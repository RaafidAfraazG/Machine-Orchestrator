#include "mo/scheduler.hpp"
#include <algorithm>
#include <iostream>
#include <pthread.h>
#include <sched.h>
namespace mo {
bool applyRealtime(RealtimeConfig c) {
  if (!c.enabled)
    return true;
  sched_param p{};
  p.sched_priority = c.priority;
  int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
  if (e) {
    static std::once_flag warned;
    std::call_once(warned, [e] {
      std::cerr << "Warning: SCHED_FIFO unavailable (error " << e
                << "); using normal scheduling\n";
    });
    return false;
  }
  return true;
}
Scheduler::Scheduler() : Scheduler(Config{}) {}
Scheduler::Scheduler(Config c, Clock clock)
    : config_(c), clock_(std::move(clock)) {
  if (!config_.workers)
    config_.workers = 1;
}
Scheduler::~Scheduler() { stop(); }
void Scheduler::start() {
  std::lock_guard<std::mutex> l(mutex_);
  if (!threads_.empty())
    return;
  stopping_ = false;
  for (size_t i = 0; i < config_.workers; ++i)
    threads_.emplace_back([this] { worker(); });
}
void Scheduler::stop() {
  {
    std::lock_guard<std::mutex> l(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto &t : threads_)
    t.join();
  threads_.clear();
}
bool Scheduler::enqueue(int priority, std::function<void()> t) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (stopping_)
      return false;
    tasks_.push_back({priority, sequence_++, clock_(), std::move(t)});
  }
  cv_.notify_one();
  return true;
}
size_t Scheduler::queued() const {
  std::lock_guard<std::mutex> l(mutex_);
  return tasks_.size();
}
void Scheduler::worker() {
  applyRealtime(config_.realtime);
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock<std::mutex> l(mutex_);
      cv_.wait(l, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty() && stopping_)
        return;
      auto now = clock_();
      auto score = [&](const Task &t) {
        int64_t age = 0;
        if (config_.agingInterval.count() > 0)
          age = std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                      t.since)
                    .count() /
                config_.agingInterval.count();
        return static_cast<int64_t>(t.priority) + age;
      };
      auto best = tasks_.begin();
      for (auto i = std::next(tasks_.begin()); i != tasks_.end(); ++i)
        if (score(*i) > score(*best) ||
            (score(*i) == score(*best) && i->sequence < best->sequence))
          best = i;
      work = std::move(best->run);
      tasks_.erase(best);
    }
    work();
  }
}
} // namespace mo
