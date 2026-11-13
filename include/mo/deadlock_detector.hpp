#pragma once
#include "mo/resource_manager.hpp"
#include <map>
#include <memory>
#include <mutex>
namespace mo {
class EventLog;
struct WorkflowInfo {
  int priority = 0;
  uint64_t birth = 0;
};
class VictimPolicy {
public:
  virtual ~VictimPolicy() = default;
  virtual WorkflowId
  choose(const std::vector<WorkflowId> &,
         const std::map<WorkflowId, WorkflowInfo> &) const = 0;
};
class LowestPriorityYoungest final : public VictimPolicy {
public:
  WorkflowId choose(const std::vector<WorkflowId> &,
                    const std::map<WorkflowId, WorkflowInfo> &) const override;
};
struct Deadlock {
  std::vector<WorkflowId> cycle;
  WorkflowId victim = 0;
};
class DeadlockDetector {
public:
  explicit DeadlockDetector(EventLog *log = nullptr,
                            std::unique_ptr<VictimPolicy> policy =
                                std::make_unique<LowestPriorityYoungest>());
  void registerWorkflow(WorkflowId, int priority, uint64_t birth);
  Deadlock block(WorkflowId waiter, WorkflowId owner, int resource);
  void unblock(WorkflowId);
  void remove(WorkflowId);
  size_t edgeCount() const;

private:
  struct Edge {
    WorkflowId owner;
    int resource;
  };
  mutable std::mutex mutex_;
  std::map<WorkflowId, WorkflowInfo> info_;
  std::map<WorkflowId, Edge> edges_;
  EventLog *log_;
  std::unique_ptr<VictimPolicy> policy_;
};
} // namespace mo
