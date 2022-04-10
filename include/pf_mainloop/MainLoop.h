//
// Created by xflajs00 on 27.10.2021.
//
/**
 * @file MainLoop.h
 * @brief A global mainloop with main thread synchronization functionality.
 * @author Petr Flajšingr
 * @date 27.10.21
 */

#ifndef PF_MAINLOOP_MAINLOOP_H
#define PF_MAINLOOP_MAINLOOP_H

#include <cassert>
#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <queue>
#include <set>
#include <thread>
#include <mutex>

namespace pf {

namespace details {
/**
 * @brief Task to be run at certain time.
 */
struct DelayedTask {
  inline DelayedTask(std::invocable auto &&fnc, const std::chrono::steady_clock::time_point &execTime)
      : fnc(std::forward<decltype(fnc)>(fnc)), execTime(execTime) {}
  std::function<void()> fnc;
  std::chrono::steady_clock::time_point execTime;

  inline bool operator>(const DelayedTask &rhs) const;

  inline void operator()() const;
};
/**
 * @brief Task to be run repeatedly.
 */
struct RepeatedTask {
  inline RepeatedTask(std::invocable auto &&fnc, std::chrono::milliseconds period)
      : fnc(std::forward<decltype(fnc)>(fnc)), execTime(std::chrono::steady_clock::now() + period), period(period) {}

  static inline std::size_t IdCounter{};
  std::function<void()> fnc;
  std::chrono::steady_clock::time_point execTime;
  std::chrono::milliseconds period;
  std::size_t id = IdCounter++;

  inline bool operator<(const RepeatedTask &rhs) const;

  inline void operator()();
};
}// namespace details

/**
 * @brief An unsubscriber class to cancel repeated tasks.
 */
class RepeatCancel {
 public:
  RepeatCancel(std::invocable auto &&fnc) : cancelFnc(std::forward<decltype(fnc)>(fnc)) {}

  /**
   * @brief Remove a repeated task from running in the main loop.
   */
  inline void cancel();
 private:
  std::function<void()> cancelFnc;
};

/**
 * @brief Mainloop implementation. Allows the user to enqueue a task to be run in the next loop or after some time delay.
 * Tasks called periodically are also supported.
 */
class MainLoop {
 public:
  /**
   * Get global instance of the MainLoop.
   * @return global instance of MainLoop
   */
  static inline const std::shared_ptr<MainLoop> &Get();

  /**
   * Set a function to be called on every loop.
   * @param fnc
   */
  void setOnMainLoop(std::invocable<std::chrono::nanoseconds> auto &&fnc) {
    mainLoopFnc = std::forward<decltype(fnc)>(fnc);
  }
  /**
   * Set a function to be called after ending the loop.
   * @param fnc
   */
  void setOnDone(std::invocable auto &&fnc) {
    onDoneFnc = std::forward<decltype(fnc)>(fnc);
  }

  /**
   * Get total runtime since MainLoop::run() has been called/
   * @remark If MainLoop::run() wasn't called then the result is undefined.
   * @return time difference between now and a call to MainLoop::run()
   */
  inline std::chrono::nanoseconds getRuntime() const;

  /**
   * Start the loop. Blocks current thread and considers it as the main one.
   */
  inline void run();
  /**
   * Set a flag to stop the loop. Current loop will finish and it'll stop in the next one.
   */
  inline void stop();

  /**
   * Enqueue a task. If the task is enqueued from the main thread it'll be executed immediately,
   * otherwise it'll be executed in the next loop.
   * @param fnc task to be executed
   */
  void enqueue(std::invocable auto &&fnc) {
    if (std::this_thread::get_id() == mainThreadId) {
      fnc();
    } else {
      enqueue(std::forward<decltype(fnc)>(fnc), std::chrono::milliseconds{0});
    }
  }
  /**
   * Enqueue a task. The task'll be executed in the next loop.
   * @param fnc task to be executed
   */
  void forceEnqueue(std::invocable auto &&fnc) {
    enqueue(std::forward<decltype(fnc)>(fnc), std::chrono::milliseconds{0});
  }

  /**
   * Enqueue a task to be executed after time delay. The task will be executed in the main thread.
   * @param fnc task to be executed
   * @param delay time delay
   */
  void enqueue(std::invocable auto &&fnc, std::chrono::milliseconds delay) {
    auto lock = std::scoped_lock{queueMtx};
    const auto execTime = std::chrono::steady_clock::now() + delay;
    taskQueue.emplace(std::forward<decltype(fnc)>(fnc), execTime);
  }

  /**
   * Add a task which'll be executed periodically on the main thread.
   * @param fnc task to be executed periodically
   * @param period time period between executions
   * @return a structure which allows for stopping the periodic task
   */
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

  std::mutex queueMtx;
  std::priority_queue<details::DelayedTask, std::vector<details::DelayedTask>, std::greater<>> taskQueue;
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
  auto lock = std::scoped_lock{queueMtx};
  while (!taskQueue.empty() && taskQueue.top().execTime <= currentTime) {
    taskQueue.top()();
    taskQueue.pop();
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
