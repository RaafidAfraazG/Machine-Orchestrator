#include "mo/resource_manager.hpp"
#include "mo/deadlock_detector.hpp"
#include "mo/event_log.hpp"
#include <algorithm>
namespace mo {
ResourceManager::ResourceManager(LockMode mode, EventLog *log)
    : mode_(mode), log_(log) {}
void ResourceManager::attachDetector(DeadlockDetector *d) {
  std::lock_guard<std::mutex> l(mutex_);
  detector_ = d;
}
AcquireResult ResourceManager::acquire(WorkflowId id,
                                       const std::vector<int> &devices,
                                       int priority, uint64_t birth,
                                       std::function<void()> wake) {
  std::lock_guard<std::mutex> l(mutex_);
  auto wanted = devices;
  if (mode_ == LockMode::Ordered)
    std::sort(wanted.begin(), wanted.end());
  auto &own = held_[id];
  if (mode_ == LockMode::Ordered)
    for (int device : wanted)
      if (!own.count(device) && !own.empty() && device < *own.rbegin()) {
        if (log_)
          log_->record("rank_violation", id, device);
        return {AcquireStatus::RankViolation, {}, 0};
      }
  if (detector_) {
    detector_->registerWorkflow(id, priority, birth);
    detector_->unblock(id);
  }
  waits_.erase(id);
  for (int device : wanted) {
    auto owner = owners_.find(device);
    if (owner != owners_.end() && owner->second != id) {
      waits_[id] = {device, owner->second, std::move(wake)};
      AcquireResult r{AcquireStatus::Blocked, {}, 0};
      if (detector_) {
        auto d = detector_->block(id, owner->second, device);
        r.cycle = d.cycle;
        r.victim = d.victim;
      }
      return r;
    }
    if (own.insert(device).second) {
      owners_[device] = id;
      if (log_)
        log_->record("lock_acquire", id, device);
    }
  }
  return {};
}
void ResourceManager::release(WorkflowId id) {
  std::vector<std::function<void()>> wakes;
  {
    std::lock_guard<std::mutex> l(mutex_);
    waits_.erase(id);
    if (detector_)
      detector_->remove(id);
    auto h = held_.find(id);
    if (h != held_.end()) {
      for (int device : h->second) {
        owners_.erase(device);
        if (log_)
          log_->record("lock_release", id, device);
      }
      held_.erase(h);
    }
    for (auto i = waits_.begin(); i != waits_.end();) {
      if (i->second.owner == id) {
        if (detector_)
          detector_->unblock(i->first);
        wakes.push_back(std::move(i->second.wake));
        i = waits_.erase(i);
      } else
        ++i;
    }
  }
  for (auto &wake : wakes)
    if (wake)
      wake();
}
bool ResourceManager::allFree() const {
  std::lock_guard<std::mutex> l(mutex_);
  return owners_.empty() && waits_.empty();
}
std::set<int> ResourceManager::held(WorkflowId id) const {
  std::lock_guard<std::mutex> l(mutex_);
  auto i = held_.find(id);
  return i == held_.end() ? std::set<int>{} : i->second;
}
} // namespace mo
