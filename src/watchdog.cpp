#include "mo/watchdog.hpp"
#include <algorithm>
namespace mo {
Watchdog::Watchdog(Reactor &r, WatchdogConfig c, Transition t, ReconnectProbe p)
    : reactor_(r), config_(c), change_(std::move(t)), probe_(std::move(p)),
      backoff_(c.initialBackoff) {
  if (backoff_.count() < 1)
    backoff_ = std::chrono::milliseconds(1);
  if (config_.maxBackoff < backoff_)
    config_.maxBackoff = backoff_;
  if (!config_.failureThreshold)
    config_.failureThreshold = 1;
}
void Watchdog::transition(Health h) {
  if (health_ != h) {
    health_ = h;
    change_(h);
  }
}
void Watchdog::observe(bool success) {
  if (stopped_ || health_ == Health::Faulted || health_ == Health::Recovering)
    return;
  if (success) {
    failures_ = 0;
    transition(Health::Healthy);
    return;
  }
  ++failures_;
  transition(Health::Suspect);
  if (failures_ >= config_.failureThreshold) {
    transition(Health::Faulted);
    schedule();
  }
}
void Watchdog::schedule() {
  auto weak = weak_from_this();
  timer_ = reactor_.after(backoff_, [weak] {
    if (auto self = weak.lock())
      self->retry();
  });
  backoff_ = std::min(backoff_ * 2, config_.maxBackoff);
}
void Watchdog::retry() {
  if (stopped_)
    return;
  timer_ = 0;
  transition(Health::Recovering);
  auto weak = weak_from_this();
  auto generation = ++generation_;
  probe_([weak, generation](bool ok) {
    if (auto self = weak.lock()) {
      if (self->stopped_ || generation != self->generation_)
        return;
      if (ok) {
        self->failures_ = 0;
        self->backoff_ = std::max(std::chrono::milliseconds(1),
                                  self->config_.initialBackoff);
        self->transition(Health::Healthy);
      } else {
        self->transition(Health::Faulted);
        self->schedule();
      }
    }
  });
}
void Watchdog::stop() {
  stopped_ = true;
  ++generation_;
  reactor_.cancel(timer_);
}
} // namespace mo
