#include "mo/deadlock_detector.hpp"
#include "mo/event_log.hpp"
#include <algorithm>
#include <sstream>
namespace mo {
WorkflowId LowestPriorityYoungest::choose(
    const std::vector<WorkflowId> &cycle,
    const std::map<WorkflowId, WorkflowInfo> &info) const {
  auto victim = cycle.front();
  for (auto id : cycle) {
    auto a = info.at(id), b = info.at(victim);
    if (a.priority < b.priority ||
        (a.priority == b.priority &&
         (a.birth > b.birth || (a.birth == b.birth && id > victim))))
      victim = id;
  }
  return victim;
}
DeadlockDetector::DeadlockDetector(EventLog *log,
                                   std::unique_ptr<VictimPolicy> policy)
    : log_(log), policy_(std::move(policy)) {
  if (!policy_)
    policy_ = std::make_unique<LowestPriorityYoungest>();
}
void DeadlockDetector::registerWorkflow(WorkflowId id, int p, uint64_t birth) {
  std::lock_guard<std::mutex> l(mutex_);
  info_[id] = {p, birth};
}
Deadlock DeadlockDetector::block(WorkflowId waiter, WorkflowId owner,
                                 int resource) {
  std::lock_guard<std::mutex> l(mutex_);
  info_.try_emplace(waiter);
  info_.try_emplace(owner);
  edges_[waiter] = {owner, resource};
  std::vector<WorkflowId> path;
  std::map<WorkflowId, size_t> seen;
  auto id = waiter;
  while (edges_.count(id)) {
    auto found = seen.find(id);
    if (found != seen.end()) {
      Deadlock d;
      d.cycle.assign(path.begin() + static_cast<std::ptrdiff_t>(found->second),
                     path.end());
      d.victim = policy_->choose(d.cycle, info_);
      if (log_) {
        std::ostringstream text;
        for (auto member : d.cycle)
          text << member << " --device " << edges_.at(member).resource << "--> "
               << edges_.at(member).owner << "; ";
        text << "victim=" << d.victim;
        log_->record("deadlock_detected", d.victim, -1, 0, text.str());
      }
      return d;
    }
    seen[id] = path.size();
    path.push_back(id);
    id = edges_.at(id).owner;
  }
  return {};
}
void DeadlockDetector::unblock(WorkflowId id) {
  std::lock_guard<std::mutex> l(mutex_);
  edges_.erase(id);
}
void DeadlockDetector::remove(WorkflowId id) {
  std::lock_guard<std::mutex> l(mutex_);
  edges_.erase(id);
  info_.erase(id);
  for (auto i = edges_.begin(); i != edges_.end();)
    if (i->second.owner == id)
      i = edges_.erase(i);
    else
      ++i;
}
size_t DeadlockDetector::edgeCount() const {
  std::lock_guard<std::mutex> l(mutex_);
  return edges_.size();
}
} // namespace mo
