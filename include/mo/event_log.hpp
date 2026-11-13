#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>
namespace mo {
struct Event {
  uint64_t timestampNs;
  std::string type;
  uint64_t workflow;
  int device;
  uint64_t command;
  std::string detail;
};
class EventLog {
public:
  explicit EventLog(size_t capacity = 65536) : capacity_(capacity) {}
  void record(std::string type, uint64_t workflow = 0, int device = -1,
              uint64_t command = 0, std::string detail = {});
  std::vector<Event> snapshot() const;
  void dumpCsv(std::ostream &) const;

private:
  size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<Event> events_;
};
} // namespace mo
