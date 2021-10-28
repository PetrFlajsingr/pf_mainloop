# pf_mainloop
A simple main loop implementation.

Example usage:

```cpp
pf::MainLoop::Get()->setOnMainLoop(
    [] (std::chrono::nanosecond frameTime) {
      // main loop code
      if (shouldExit) {
        pf::MainLoop::Get()->stop();
      }
    });
pf::MainLoop::Get()->setOnDone(
    []{
      std::cout << "Main loop ended" << std::endl;
    });

pf::MainLoop::Get()->run(); // blocking operation
```

The class also supports executing delayed tasks, tasks can be enqueued from a different thread and they'll be run in the main loop's thread:
```cpp
// if enqueued from main loop's thread the function will be called immediately, 
// otherwise it'll run in the next loop
pf::MainLoop::Get()->enqueue([] {
  std::cout << "Enqueued task" << std::endl;
});
// this function will be called in main loop's thread in ~2 seconds
const auto delay = std::chrono::seconds{2};
pf::MainLoop::Get()->enqueue([] {
  std::cout << "Enqueued delayed task" << std::endl;
}, delay);
```

Periodic tasks are also supported, these have to be registered from the same thread as the main loop's thread.
```cpp
const auto period = std::chrono::seconds{5};
pf::MainLoop::Get()->repeat([] {
  std::cout << "Repeated task" << std::endl;
}, period);
```