#include "mo/engine.hpp"
#include <arpa/inet.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using namespace mo;
using namespace std::chrono_literals;
namespace {
bool silence(int port, int id) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return false;
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  std::string command = "silence " + std::to_string(id) + "\n";
  if (ok)
    ok = send(fd, command.data(), command.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(command.size());
  char reply[8]{};
  if (ok)
    ok = recv(fd, reply, sizeof(reply), 0) >= 3 &&
         std::string(reply, 3) == "OK\n";
  close(fd);
  return ok;
}
} // namespace
int main(int argc, char **argv) {
  int count = 40, port = 15020, kill = -1, control = 0;
  bool unchecked = false;
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--unchecked-deadlock-demo") {
        unchecked = true;
        continue;
      }
      if (i + 1 == argc)
        return 2;
      int value = std::stoi(argv[++i]);
      if (arg == "--devices")
        count = value;
      else if (arg == "--base-port")
        port = value;
      else if (arg == "--kill-device")
        kill = value;
      else if (arg == "--control-port")
        control = value;
      else
        return 2;
    }
  } catch (...) {
    return 2;
  }
  if (count < 1 || port < 2 || port > 65535 || count > 65536 - port ||
      kill >= count || kill < -1 || (unchecked && count < 2))
    return 2;
  EngineConfig config;
  if (unchecked)
    config.locking = LockMode::Unchecked;
  config.requestTimeout = 150ms;
  config.watchdog.initialBackoff = 500ms;
  Engine engine(config);
  for (int i = 0; i < count; ++i)
    engine.addDevice(
        i, {"127.0.0.1", static_cast<uint16_t>(port + i), {}, 115200});
  if (!engine.start())
    return 1;
  std::vector<WorkflowHandle> handles;
  if (unchecked) {
    engine.resources().acquire(10001, {0}, 10, 1, {});
    engine.resources().acquire(10002, {1}, 1, 2, {});
    Workflow a;
    a.withId(10001)
        .named("cycle-A")
        .withPriority(10)
        .needs({0, 1})
        .manualLocks()
        .then(AcquireDevices{{1}})
        .then(ReadRegisters{0, 0, 1, "a"});
    Workflow b;
    b.withId(10002)
        .named("cycle-B")
        .withPriority(1)
        .needs({0, 1})
        .manualLocks()
        .then(AcquireDevices{{0}})
        .then(ReadRegisters{1, 0, 1, "b"});
    handles.push_back(engine.submit(a));
    handles.push_back(engine.submit(b));
  } else {
    for (int i = 0; i < 100; ++i) {
      int a = i % count, b = (a + 1) % count;
      Workflow w;
      w.named("workflow-" + std::to_string(i))
          .withPriority(i % 7)
          .needs({a, b});
      if (i == 0 && kill >= 0) {
        w.needs({kill}).withPriority(100).then(Delay{1000ms});
        a = kill;
      }
      w.then(WriteRegister{a, static_cast<uint16_t>(i),
                           static_cast<uint16_t>(1000 + i)})
          .then(WriteRegisters{a, static_cast<uint16_t>(200 + 2 * i), {1, 2}})
          .then(ReadRegisters{a, static_cast<uint16_t>(i), 1, "value"})
          .then(Conditional{"value", 0, Comparison::Equal,
                            static_cast<uint16_t>(1000 + i), Branch::Continue,
                            Branch::Fail, 0});
      handles.push_back(engine.submit(std::move(w)));
    }
    if (kill >= 0 && !silence(control ? control : port - 1, kill)) {
      std::cerr << "Control failed: start simulator with --control-port "
                << (control ? control : port - 1) << '\n';
      engine.stop();
      return 1;
    }
  }
  bool finished = true;
  for (auto &h : handles)
    if (h.completion.wait_for(30s) != std::future_status::ready)
      finished = false;
  if (unchecked)
    for (const auto &event : engine.events().snapshot())
      if (event.type == "deadlock_detected")
        std::cout << "Deadlock: " << event.detail << '\n';
  auto stats = engine.stats();
  std::cout << "completed=" << stats.completed << " failed=" << stats.failed
            << " aborted=" << stats.aborted
            << " deadlocks_resolved=" << stats.deadlocksResolved
            << " devices_faulted=" << stats.devicesFaulted << '\n';
  engine.stop();
  if (!finished || !engine.resources().allFree())
    return 1;
  if (unchecked)
    return stats.completed == 1 && stats.aborted == 1 &&
                   stats.deadlocksResolved == 1
               ? 0
               : 1;
  if (kill >= 0)
    return stats.completed > 0 && stats.failed > 0 && stats.devicesFaulted == 1
               ? 0
               : 1;
  return stats.completed == 100 && stats.failed == 0 && stats.aborted == 0 ? 0
                                                                           : 1;
}
