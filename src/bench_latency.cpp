#include "mo/engine.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
using namespace mo;
int main(int argc, char **argv) {
  int devices = 40, workflows = 500, port = 15020;
  double duration = 1;
  bool fifo = false;
  size_t capacity = 1000000;
  std::string path = "latency.csv";
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--fifo") {
        fifo = true;
        continue;
      }
      if (i + 1 >= argc)
        return 2;
      std::string value = argv[++i];
      if (arg == "--devices")
        devices = std::stoi(value);
      else if (arg == "--workflows")
        workflows = std::stoi(value);
      else if (arg == "--base-port")
        port = std::stoi(value);
      else if (arg == "--duration")
        duration = std::stod(value);
      else if (arg == "--event-capacity")
        capacity = std::stoull(value);
      else if (arg == "--csv")
        path = value;
      else
        return 2;
    }
  } catch (...) {
    return 2;
  }
  if (devices < 1 || workflows < 1 || port < 1 || port > 65535 ||
      devices > 65536 - port || !std::isfinite(duration) || duration < 0)
    return 2;
  EngineConfig config;
  config.eventCapacity = capacity;
  config.scheduler.realtime = {fifo, 1};
  Engine engine(config);
  for (int i = 0; i < devices; ++i)
    if (!engine.addDevice(
            i, {"127.0.0.1", static_cast<uint16_t>(port + i), {}, 115200}))
      return 1;
  if (!engine.start())
    return 1;
  auto until = std::chrono::steady_clock::now() +
               std::chrono::duration<double>(duration);
  size_t completed = 0, failed = 0;
  do {
    std::vector<WorkflowHandle> batch;
    for (int i = 0; i < workflows; ++i) {
      int id = i % devices;
      Workflow w;
      w.needs({id}).then(ReadRegisters{id, 0, 1, "value"});
      batch.push_back(engine.submit(std::move(w)));
    }
    for (auto &handle : batch) {
      if (handle.completion.wait_for(std::chrono::seconds(15)) !=
          std::future_status::ready) {
        engine.stop();
        return 1;
      }
      if (handle.completion.get().state == WorkflowState::Completed)
        ++completed;
      else
        ++failed;
    }
  } while (std::chrono::steady_clock::now() < until);
  engine.stop();
  auto events = engine.events().snapshot();
  size_t granted = 0, fallback = 0;
  for (auto &e : events)
    if (e.type == "realtime_worker" || e.type == "realtime_reactor") {
      if (e.detail == "granted")
        ++granted;
      else if (e.detail == "fallback")
        ++fallback;
    }
  std::ofstream csv(path);
  if (!csv)
    return 1;
  csv << "# realtime_requested=" << fifo << " granted=" << granted
      << " fallback=" << fallback << '\n';
  engine.events().dumpCsv(csv);
  if (!csv)
    return 1;
  std::cout << "completed=" << completed << " failed=" << failed
            << " realtime_requested=" << fifo << " granted=" << granted
            << " fallback=" << fallback
            << " dropped=" << engine.events().dropped() << '\n';
  return failed ? 1 : 0;
}
