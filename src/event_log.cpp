#include "mo/event_log.hpp"
#include "mo/bug_injection.hpp"
namespace mo {
void EventLog::record(std::string type, uint64_t workflow, int device,
                      uint64_t command, std::string detail) {
  std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
  if (!MO_BUG("unlocked_log"))
    lock.lock();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
  Event event{
      static_cast<uint64_t>(ns), std::move(type), workflow, device, command,
      std::move(detail)};
  if (sink_)
    sink_(event);
  if (!capacity_) {
    if (!MO_BUG("drop_counter"))
      ++dropped_;
    return;
  }
  if (events_.size() == capacity_) {
    events_.pop_front();
    if (!MO_BUG("drop_counter"))
      ++dropped_;
  }
  events_.push_back(std::move(event));
}
std::vector<Event> EventLog::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {events_.begin(), events_.end()};
}
void EventLog::dumpCsv(std::ostream &out) const {
  out << "# dropped_events=" << dropped() << "\n";
  out << "timestamp_ns,type,workflow_id,device_id,command_id,detail\n";
  auto quote = [&out](const std::string &s) {
    out << '"';
    for (char c : s) {
      if (c == '"')
        out << '"';
      out << c;
    }
    out << '"';
  };
  for (const auto &e : snapshot()) {
    out << e.timestampNs << ',';
    quote(e.type);
    out << ',' << e.workflow << ',' << e.device << ',' << e.command << ',';
    quote(e.detail);
    out << '\n';
  }
}
uint64_t EventLog::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}
void EventLog::setSink(std::function<void(const Event &)> sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = std::move(sink);
}
} // namespace mo
