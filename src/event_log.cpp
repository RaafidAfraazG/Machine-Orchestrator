#include "mo/event_log.hpp"
namespace mo {
void EventLog::record(std::string type, uint64_t workflow, int device,
                      uint64_t command, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!capacity_)
    return;
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
  if (events_.size() == capacity_)
    events_.pop_front();
  events_.push_back({static_cast<uint64_t>(ns), std::move(type), workflow,
                     device, command, std::move(detail)});
}
std::vector<Event> EventLog::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {events_.begin(), events_.end()};
}
void EventLog::dumpCsv(std::ostream &out) const {
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
} // namespace mo
