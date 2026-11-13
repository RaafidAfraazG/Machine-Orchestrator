#pragma once
#include "mo/deadlock_detector.hpp"
#include "mo/device.hpp"
#include "mo/event_log.hpp"
#include "mo/scheduler.hpp"
#include "mo/watchdog.hpp"
#include "mo/workflow.hpp"
#include <atomic>
#include <memory>
namespace mo {
struct Endpoint {
  std::string host = "127.0.0.1";
  uint16_t port = 15020;
  std::string serialPath;
  unsigned baud = 115200;
};
struct EngineConfig {
  Scheduler::Config scheduler;
  WatchdogConfig watchdog;
  LockMode locking = LockMode::Ordered;
  std::chrono::milliseconds requestTimeout{500};
  size_t eventCapacity = 65536;
};
struct EngineStats {
  size_t completed = 0, failed = 0, aborted = 0, deadlocksResolved = 0,
         devicesFaulted = 0;
};
class Engine {
public:
  Engine();
  explicit Engine(EngineConfig, std::unique_ptr<VictimPolicy> policy =
                                    std::make_unique<LowestPriorityYoungest>());
  ~Engine();
  bool addDevice(int id, Endpoint);
  bool start();
  WorkflowHandle submit(Workflow);
  void stop();
  EventLog &events() { return log_; }
  ResourceManager &resources() {
    return resources_;
  } // mutations external to Engine: UNCHECKED test/demo only
  EngineStats stats() const;
  Health health(int id) const;
  // External-owner wait, never call from a worker or reactor callback.
  bool waitForHealth(int id, Health, std::chrono::milliseconds timeout);

private:
  struct Run {
    Workflow workflow;
    WorkflowState state = WorkflowState::Pending;
    size_t step = 0;
    Variables variables;
    std::promise<WorkflowResult> promise;
    bool terminal = false, waiting = false, acquired = false;
    uint64_t birth = 0, delayTimer = 0;
  };
  void schedule(const std::shared_ptr<Run> &);
  void advance(const std::shared_ptr<Run> &);
  void finish(const std::shared_ptr<Run> &, WorkflowState, WorkflowReason,
              std::string);
  bool acquire(const std::shared_ptr<Run> &, const std::vector<int> &);
  void state(const std::shared_ptr<Run> &, WorkflowState);
  void healthChanged(int, Health);
  void response(const std::shared_ptr<Run> &, int, Result<Response>,
                std::string variable);
  bool valid(const Workflow &) const;
  void barrier();
  EngineConfig config_;
  Reactor reactor_;
  EventLog log_;
  Scheduler scheduler_;
  DeadlockDetector detector_;
  ResourceManager resources_;
  mutable std::mutex mutex_;
  std::condition_variable healthCv_;
  std::map<int, std::shared_ptr<Device>> devices_;
  std::map<int, std::shared_ptr<Watchdog>> watchdogs_;
  std::map<int, Health> health_;
  std::map<WorkflowId, std::shared_ptr<Run>> runs_;
  WorkflowId nextId_ = 1;
  uint64_t birth_ = 0;
  std::set<int> everFaulted_;
  uint64_t nextCommand_ = 1;
  EngineStats stats_;
  bool started_ = false, stopping_ = false, finished_ = false;
};
} // namespace mo
