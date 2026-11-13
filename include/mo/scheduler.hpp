#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace mo {
struct RealtimeConfig {
  bool enabled = false;
  int priority = 1;
};
// Returns false on fallback; warnings are emitted once process-wide.
bool applyRealtime(RealtimeConfig);
class Scheduler {
public:
  struct Config {
    size_t workers = 4;
    std::chrono::milliseconds agingInterval{50};
    RealtimeConfig realtime;
    std::function<void(bool)> realtimeObserver;
  };
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  Scheduler();
  explicit Scheduler(
      Config, Clock clock = [] { return std::chrono::steady_clock::now(); });
  ~Scheduler();
  void start();
  void stop();
  bool enqueue(int priority, std::function<void()>);
  size_t queued() const;

private:
  struct Task {
    int priority;
    uint64_t sequence;
    std::chrono::steady_clock::time_point since;
    std::function<void()> run;
  };
  void worker();
  Config config_;
  Clock clock_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Task> tasks_;
  std::vector<std::thread> threads_;
  uint64_t sequence_ = 0;
  bool stopping_ = false;
};
} // namespace mo
