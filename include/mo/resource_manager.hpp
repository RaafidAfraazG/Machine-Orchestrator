#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
namespace mo {
class DeadlockDetector;
class EventLog;
using WorkflowId = uint64_t;
enum class LockMode { Ordered, Unchecked };
enum class AcquireStatus { Granted, Blocked, RankViolation };
struct AcquireResult {
  AcquireStatus status = AcquireStatus::Granted;
  std::vector<WorkflowId> cycle;
  WorkflowId victim = 0;
};
class ResourceManager {
public:
  explicit ResourceManager(LockMode mode = LockMode::Ordered,
                           EventLog *log = nullptr);
  void attachDetector(DeadlockDetector *detector);
  // Callback only makes work runnable; caller retries acquire after
  // notification.
  AcquireResult acquire(WorkflowId, const std::vector<int> &, int priority,
                        uint64_t birth, std::function<void()> wake);
  void release(WorkflowId); // also cancels a pending acquisition, idempotently
  bool allFree() const;
  std::set<int> held(WorkflowId) const;
  LockMode mode() const { return mode_; }

private:
  struct Wait {
    int resource;
    WorkflowId owner;
    std::function<void()> wake;
  };
  LockMode mode_;
  EventLog *log_;
  DeadlockDetector *detector_ = nullptr;
  mutable std::mutex mutex_;
  std::map<int, WorkflowId> owners_;
  std::map<WorkflowId, std::set<int>> held_;
  std::map<WorkflowId, Wait> waits_;
};
} // namespace mo
