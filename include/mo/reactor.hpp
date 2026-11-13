#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
namespace mo {
class Reactor {
public:
  using Task = std::function<void()>;
  struct Callbacks {
    Task onReadable, onWritable, onError;
  };
  Reactor();
  ~Reactor();
  Reactor(const Reactor &) = delete;
  Reactor &operator=(const Reactor &) = delete;
  bool valid() const { return epoll_ >= 0 && wake_ >= 0; }
  void start();
  // Stops after draining posted work. Producers must stop submitting first.
  void stop();
  void join();
  void post(Task);
  // Registration and timer operations are loop-thread only (or before start).
  bool watch(int fd, bool writable, Callbacks);
  bool modify(int fd, bool writable);
  void unwatch(int fd);
  uint64_t after(std::chrono::milliseconds, Task);
  void cancel(uint64_t);

private:
  void run();
  int epoll_ = -1, wake_ = -1;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::queue<Task> posts_;
  struct Watch {
    int fd;
    Callbacks callbacks;
  };
  uint64_t nextWatch_ = 1;
  std::unordered_map<uint64_t, Watch> watches_;
  std::unordered_map<int, uint64_t> ids_;
  using Key = std::pair<std::chrono::steady_clock::time_point, uint64_t>;
  std::map<Key, Task> timers_;
  std::unordered_map<uint64_t, Key> timerKeys_;
  uint64_t nextTimer_ = 1;
};
} // namespace mo
