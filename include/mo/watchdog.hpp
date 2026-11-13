#pragma once
#include "mo/reactor.hpp"
#include <cstdint>
#include <functional>
#include <memory>
namespace mo {
enum class Health { Healthy, Suspect, Faulted, Recovering };
struct WatchdogConfig {
  unsigned failureThreshold = 2;
  std::chrono::milliseconds initialBackoff{100}, maxBackoff{5000},
      probeTimeout{200};
  uint16_t probeRegister = 0;
  bool failFast = true;
};
// All methods and hooks are reactor-thread only. ReconnectProbe must call its
// completion on the reactor; stop invalidates any outstanding probe generation.
class Watchdog : public std::enable_shared_from_this<Watchdog> {
public:
  using Transition = std::function<void(Health)>;
  using ReconnectProbe = std::function<void(std::function<void(bool)>)>;
  Watchdog(Reactor &, WatchdogConfig, Transition, ReconnectProbe);
  void observe(bool success);
  void stop();
  Health health() const { return health_; }

private:
  void transition(Health);
  void retry();
  void schedule();
  Reactor &reactor_;
  WatchdogConfig config_;
  Transition change_;
  ReconnectProbe probe_;
  Health health_ = Health::Healthy;
  unsigned failures_ = 0;
  uint64_t timer_ = 0, generation_ = 0;
  std::chrono::milliseconds backoff_;
  bool stopped_ = false;
};
} // namespace mo
