//
// Created by xflajs00 on 27.10.2021.
//

#ifndef PF_MAINLOOP_MAINLOOP_H
#define PF_MAINLOOP_MAINLOOP_H

#include <cassert>
#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <pf_common/coroutines/Sequence.h>
#include <pf_common/parallel/Safe.h>
#include <queue>
#include <set>
#include <thread>

namespace pf {

namespace details {
struct DelayedTask {
  inline DelayedTask(std::invocable auto &&fnc, const std::chrono::steady_clock::time_point &execTime)
      : fnc(std::forward<decltype(fnc)>(fnc)), execTime(execTime) {}
  std::function<void()> fnc;
  std::chrono::steady_clock::time_point execTime;

  inline bool operator>(const DelayedTask &rhs) const;

  inline void operator()() const;
};

struct RepeatedTask {
  inline RepeatedTask(std::invocable auto &&fnc, std::chrono::milliseconds period)
      : fnc(std::forward<decltype(fnc)>(fnc)), execTime(std::chrono::steady_clock::now() + period), period(period) {}

  static inline auto IdGenerator = iota<std::size_t>();
  std::function<void()> fnc;
  std::chrono::steady_clock::time_point execTime;
  std::chrono::milliseconds period;
  std::size_t id = getNext(IdGenerator);

  inline bool operator<(const RepeatedTask &rhs) const;

  inline void operator()();
};
}// namespace details

class RepeatCancel {
 public:
  RepeatCancel(std::invocable auto &&fnc) : cancelFnc(std::forward<decltype(fnc)>(fnc)) {}

  inline void cancel();
 private:
  std::function<void()> cancelFnc;
};

class MainLoop {
 public:
  static inline const std::shared_ptr<MainLoop> &Get();

  void setOnMainLoop(std::invocable<std::chrono::nanoseconds> auto &&fnc) {
    mainLoopFnc = std::forward<decltype(fnc)>(fnc);
  }

  void setOnDone(std::invocable auto &&fnc) {
    onDoneFnc = std::forward<decltype(fnc)>(fnc);
  }

  inline std::chrono::nanoseconds getRuntime() const;

  inline void run();
  inline void stop();

  void enqueue(std::invocable auto &&fnc) {
    if (std::this_thread::get_id() == mainThreadId) {
      fnc();
    } else {
      enqueue(std::forward<decltype(fnc)>(fnc), std::chrono::milliseconds{0});
    }
  }

  void enqueue(std::invocable auto &&fnc, std::chrono::milliseconds delay) {
    const auto execTime = std::chrono::steady_clock::now() + delay;
    auto queueAccess = taskQueue.writeAccess();
    queueAccess->emplace(std::forward<decltype(fnc)>(fnc), execTime);
  }

  RepeatCancel repeat(std::invocable auto &&fnc, std::chrono::milliseconds period) {
    if (std::this_thread::get_id() != mainThreadId) {
      assert(false && "MainLoop::repeat should only be called from the main thread");
    }
    auto task = details::RepeatedTask(std::forward<decltype(fnc)>(fnc), period);
    const auto taskID = task.id;
    repeatedTasks.emplace(task);
    return {[this, taskID] {
      std::erase_if(repeatedTasks, [taskID](const auto &task) {
        return task.id == taskID;
      });
    }};
  }

 private:
  inline MainLoop();

  inline void callDelayedFunctions(std::chrono::steady_clock::time_point currentTime);
  inline void callRepeatedFunctions(std::chrono::steady_clock::time_point currentTime);

  std::chrono::time_point<std::chrono::steady_clock> startTime;
  std::chrono::time_point<std::chrono::steady_clock> lastTime;

  std::function<void(std::chrono::nanoseconds)> mainLoopFnc = [](auto) {};
  std::function<void()> onDoneFnc = [] {};
  Safe<std::priority_queue<details::DelayedTask, std::vector<details::DelayedTask>, std::greater<>>> taskQueue;
  std::set<details::RepeatedTask, std::less<details::RepeatedTask>> repeatedTasks;
  bool shouldStop = false;

  static inline std::shared_ptr<MainLoop> instance = nullptr;
  const std::thread::id mainThreadId;
};

const std::shared_ptr<MainLoop> &MainLoop::Get() {
  if (instance == nullptr) {
    instance = std::shared_ptr<MainLoop>(new MainLoop());
  }
  return instance;
}

std::chrono::nanoseconds MainLoop::getRuntime() const {
  return std::chrono::steady_clock::now() - startTime;
}

void MainLoop::run() {
  shouldStop = false;
  startTime = std::chrono::steady_clock::now();
  lastTime = startTime;
  while (!shouldStop) {
    const auto currentTime = std::chrono::steady_clock::now();
    const auto deltaTime = currentTime - lastTime;
    lastTime = currentTime;
    mainLoopFnc(deltaTime);
    callDelayedFunctions(currentTime);
    callRepeatedFunctions(currentTime);
  }
  onDoneFnc();
}

void MainLoop::stop() {
  shouldStop = true;
}

MainLoop::MainLoop() : mainThreadId(std::this_thread::get_id()) {}

void MainLoop::callDelayedFunctions(std::chrono::steady_clock::time_point currentTime) {
  auto queueAccess = taskQueue.writeAccess();
  while (!queueAccess->empty() && queueAccess->top().execTime <= currentTime) {
    queueAccess->top()();
    queueAccess->pop();
  }
}
void MainLoop::callRepeatedFunctions(std::chrono::steady_clock::time_point currentTime) {
  auto iter = repeatedTasks.begin();
  while (iter != repeatedTasks.end() && iter->execTime <= currentTime) {
    auto task = *iter;
    iter = repeatedTasks.erase(iter);
    task();
    task.execTime = std::chrono::steady_clock::now() + task.period;
    repeatedTasks.emplace(task);
  }
}

namespace details {
bool DelayedTask::operator>(const DelayedTask &rhs) const {
  return execTime > rhs.execTime;
}

void DelayedTask::operator()() const { fnc(); }

void RepeatedTask::operator()() {
  execTime = execTime + period;
  fnc();
}
bool
RepeatedTask::operator<(const RepeatedTask &rhs) const {
  return execTime < rhs.execTime;
}
}// namespace details

void RepeatCancel::cancel() {
  cancelFnc();
}
}// namespace pf

#endif//PF_MAINLOOP_MAINLOOP_H
