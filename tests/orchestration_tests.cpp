#include "mo/engine.hpp"
#include "port_lease.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
using namespace mo;
using namespace std::chrono_literals;
namespace {
constexpr auto Deadline = 10s;
constexpr auto RequestTimeout = 200ms;
constexpr auto Backoff = 100ms;
constexpr auto ProbeTimeout = 300ms;
constexpr auto ShortDelay = 10ms;
TEST(EventLog, BoundedMonotonicCsv) {
  EventLog log(2);
  log.record("old");
  log.record("new", 7, 2, 5, "a,\"b\"");
  log.record("last");
  auto events = log.snapshot();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, "new");
  EXPECT_LE(events[0].timestampNs, events[1].timestampNs);
  std::ostringstream csv;
  log.dumpCsv(csv);
  EXPECT_NE(csv.str().find("\"a,\"\"b\"\"\""), std::string::npos);
  EventLog disabled(0);
  disabled.record("ignored");
  EXPECT_TRUE(disabled.snapshot().empty());
}
TEST(Scheduler, PriorityAndFifo) {
  Scheduler::Config c;
  c.workers = 1;
  c.agingInterval = 0ms;
  Scheduler s(c);
  std::vector<int> order;
  std::promise<void> done;
  auto future = done.get_future();
  for (auto pair :
       std::vector<std::pair<int, int>>{{1, 1}, {9, 2}, {9, 3}, {3, 4}})
    s.enqueue(pair.first, [&, n = pair.second] {
      order.push_back(n);
      if (order.size() == 4)
        done.set_value();
    });
  s.start();
  ASSERT_EQ(future.wait_for(Deadline), std::future_status::ready);
  s.stop();
  EXPECT_EQ(order, (std::vector<int>{2, 3, 4, 1}));
}
TEST(Scheduler, AgingBeatsNewHighPriorityWork) {
  Scheduler::Config c;
  c.workers = 1;
  c.agingInterval = 10ms;
  std::atomic<int64_t> ms{0};
  Scheduler s(c, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::milliseconds(ms.load());
  });
  std::vector<int> order;
  std::promise<void> done;
  auto future = done.get_future();
  s.enqueue(-10, [&] { order.push_back(-1); });
  ms = 10000;
  for (int i = 0; i < 100; ++i)
    s.enqueue(100, [&, i] {
      order.push_back(i);
      if (order.size() == 101)
        done.set_value();
    });
  s.start();
  ASSERT_EQ(future.wait_for(Deadline), std::future_status::ready);
  s.stop();
  ASSERT_EQ(order.size(), 101u);
  EXPECT_EQ(order.front(), -1);
}
TEST(Scheduler, RealtimeFallbackAndStopDrain) {
  EXPECT_FALSE(applyRealtime({true, 0}));
  Scheduler::Config c;
  c.workers = 1;
  c.realtime = {true, 1};
  Scheduler s(c);
  std::promise<void> done;
  auto f = done.get_future();
  s.enqueue(0, [&] { done.set_value(); });
  s.start();
  s.stop();
  EXPECT_EQ(f.wait_for(Deadline), std::future_status::ready);
  EXPECT_FALSE(s.enqueue(0, [] {}));
}
TEST(Resources, OrderedContentionViolationAndIdempotentRelease) {
  ResourceManager r;
  EXPECT_EQ(r.acquire(1, {2, 1}, 0, 1, {}).status, AcquireStatus::Granted);
  EXPECT_EQ(r.held(1), (std::set<int>{1, 2}));
  bool woke = false;
  EXPECT_EQ(r.acquire(2, {2}, 0, 2, [&] { woke = true; }).status,
            AcquireStatus::Blocked);
  EXPECT_EQ(r.acquire(1, {0}, 0, 1, {}).status, AcquireStatus::RankViolation);
  r.release(1);
  r.release(1);
  EXPECT_TRUE(woke);
  EXPECT_EQ(r.acquire(2, {2}, 0, 2, {}).status, AcquireStatus::Granted);
  r.release(2);
  EXPECT_TRUE(r.allFree());
}
TEST(Deadlock, TwoWorkflowCycleAndLowestPriority) {
  EventLog log;
  DeadlockDetector detector(&log);
  ResourceManager r(LockMode::Unchecked, &log);
  r.attachDetector(&detector);
  r.acquire(1, {1}, 10, 1, {});
  r.acquire(2, {2}, 2, 2, {});
  bool woke = false;
  EXPECT_TRUE(r.acquire(1, {2}, 10, 1, [&] { woke = true; }).cycle.empty());
  auto cycle = r.acquire(2, {1}, 2, 2, {});
  EXPECT_EQ(cycle.cycle.size(), 2u);
  EXPECT_EQ(cycle.victim, 2u);
  r.release(cycle.victim);
  EXPECT_TRUE(woke);
  EXPECT_EQ(detector.edgeCount(), 0u);
  EXPECT_EQ(r.acquire(1, {2}, 10, 1, {}).status, AcquireStatus::Granted);
  r.release(1);
  EXPECT_TRUE(r.allFree());
  auto events = log.snapshot();
  auto diagnostic =
      std::find_if(events.begin(), events.end(),
                   [](const auto &e) { return e.type == "deadlock_detected"; });
  ASSERT_NE(diagnostic, events.end());
  EXPECT_NE(diagnostic->detail.find("device 1"), std::string::npos);
  EXPECT_NE(diagnostic->detail.find("device 2"), std::string::npos);
}
TEST(Deadlock, ThreeWorkflowCycleYoungestAndNoFalsePositive) {
  DeadlockDetector detector;
  ResourceManager r(LockMode::Unchecked);
  r.attachDetector(&detector);
  for (int i = 1; i <= 3; ++i)
    r.acquire(i, {i}, 5, i, {});
  EXPECT_TRUE(r.acquire(1, {2}, 5, 1, {}).cycle.empty());
  EXPECT_TRUE(r.acquire(2, {3}, 5, 2, {}).cycle.empty());
  auto cycle = r.acquire(3, {1}, 5, 3, {});
  EXPECT_EQ(cycle.cycle.size(), 3u);
  EXPECT_EQ(cycle.victim, 3u);
  r.release(3);
  EXPECT_EQ(r.acquire(2, {3}, 5, 2, {}).status, AcquireStatus::Granted);
  r.release(2);
  EXPECT_EQ(r.acquire(1, {2}, 5, 1, {}).status, AcquireStatus::Granted);
  r.release(1);
  EXPECT_TRUE(r.allFree());
}
class Simulator {
public:
  mo::test::PortLease lease;
  pid_t pid = -1;
  int output = -1;
  std::string line;
  int port, control;
  explicit Simulator(int count)
      : port(mo::test::PortLease::base), control(port - 1) {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC))
      return;
    std::vector<std::string> args{MO_PYTHON,        MO_SIM,
                                  "--tcp-count",    std::to_string(count),
                                  "--base-port",    std::to_string(port),
                                  "--control-port", std::to_string(control)};
    std::vector<char *> argv;
    for (auto &a : args)
      argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    int error =
        posix_spawn(&pid, MO_PYTHON, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    output = pipes[0];
    if (error) {
      pid = -1;
      return;
    }
    auto until = std::chrono::steady_clock::now() + Deadline;
    while (std::chrono::steady_clock::now() < until) {
      pollfd fd{output, POLLIN, 0};
      if (poll(&fd, 1, 10000) <= 0)
        break;
      char c;
      if (read(output, &c, 1) != 1 || c == '\n')
        break;
      line += c;
    }
  }
  ~Simulator() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
    }
    if (output >= 0)
      close(output);
  }
  bool command(const std::string &action, int id) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return false;
    timeval timeout{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(control);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
    auto text = action + " " + std::to_string(id) + "\n";
    if (ok)
      ok = send(fd, text.data(), text.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(text.size());
    char reply[3];
    if (ok)
      ok = recv(fd, reply, 3, MSG_WAITALL) == 3 &&
           std::string(reply, 3) == "OK\n";
    close(fd);
    return ok;
  }
  void attach(Engine &e, int n) {
    for (int i = 0; i < n; ++i)
      ASSERT_TRUE(e.addDevice(
          i, {"127.0.0.1", static_cast<uint16_t>(port + i), {}, 115200}));
  }
};
WorkflowResult complete(WorkflowHandle h) {
  EXPECT_EQ(h.completion.wait_for(Deadline), std::future_status::ready);
  return h.completion.get();
}
TEST(Workflow, AllStepsAndConditionalBranches) {
  Simulator sim(2);
  ASSERT_EQ(sim.line, "READY");
  Engine e;
  sim.attach(e, 2);
  ASSERT_TRUE(e.start());
  Workflow w;
  w.needs({0, 1})
      .then(WriteRegister{0, 10, 44})
      .then(WriteRegisters{1, 20, {3, 4, 5}})
      .then(ReadRegisters{1, 20, 3, "words"})
      .then(Delay{ShortDelay})
      .then(Conditional{"words", 1, Comparison::Equal, 4, Branch::Skip,
                        Branch::Fail, 1})
      .then(WriteRegister{0, 10, 999})
      .then(ReadRegisters{0, 10, 1, "single"})
      .then(Conditional{"single", 0, Comparison::Greater, 40, Branch::Continue,
                        Branch::Fail, 0});
  auto result = complete(e.submit(w));
  EXPECT_EQ(result.state, WorkflowState::Completed);
  EXPECT_EQ(result.variables.at("words"), (std::vector<uint16_t>{3, 4, 5}));
  EXPECT_EQ(result.variables.at("single"), (std::vector<uint16_t>{44}));
  EXPECT_TRUE(e.resources().allFree());
  auto events = e.events().snapshot();
  uint64_t command = 0;
  for (auto &event : events)
    if (event.type == "command_submitted" && event.workflow == result.id)
      command = event.command;
  EXPECT_NE(command, 0u);
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [&](auto &event) {
    return event.type == "command_sent" && event.command == command &&
           event.workflow == result.id;
  }));
}
TEST(Workflow, FailureStepValidationAndStop) {
  Simulator sim(1);
  ASSERT_EQ(sim.line, "READY");
  EngineConfig c;
  c.scheduler.realtime = {true, 1};
  Engine e(c);
  sim.attach(e, 1);
  ASSERT_TRUE(e.start());
  Workflow w;
  w.needs({0})
      .then(WriteRegister{0, 0, 123})
      .then(ReadRegisters{0, 1024, 1, "bad"});
  auto r = complete(e.submit(w));
  EXPECT_EQ(r.state, WorkflowState::Failed);
  EXPECT_EQ(r.failingStep, 1u);
  EXPECT_EQ(r.reason, WorkflowReason::RequestFailed);
  Workflow conditional;
  conditional.needs({0}).then(Conditional{"missing", 0, Comparison::Equal, 1});
  r = complete(e.submit(conditional));
  EXPECT_EQ(r.reason, WorkflowReason::ConditionFailed);
  EXPECT_EQ(r.failingStep, 0u);
  for (int i = 0; i < 2; ++i) {
    Workflow rejected;
    rejected.needs({0}).then(WriteRegisters{0, 0, {}});
    EXPECT_EQ(complete(e.submit(rejected)).reason,
              WorkflowReason::RequestFailed);
  }
  EXPECT_EQ(e.health(0), Health::Healthy);
  Workflow branchFailure;
  branchFailure.needs({0})
      .then(ReadRegisters{0, 0, 1, "value"})
      .then(Conditional{"value", 0, Comparison::Equal, 999, Branch::Continue,
                        Branch::Fail, 0});
  auto branchResult = complete(e.submit(branchFailure));
  EXPECT_EQ(branchResult.reason, WorkflowReason::ConditionFailed);
  EXPECT_EQ(branchResult.failingStep, 1u);
  Workflow invalid;
  invalid.needs({0}).then(ReadRegisters{1, 0, 1, "x"});
  EXPECT_EQ(complete(e.submit(invalid)).reason,
            WorkflowReason::InvalidWorkflow);
  Workflow delayed;
  delayed.needs({0}).then(Delay{60s});
  auto h = e.submit(delayed);
  e.stop();
  r = complete(h);
  EXPECT_EQ(r.state, WorkflowState::Aborted);
  EXPECT_EQ(r.reason, WorkflowReason::Stopped);
  EXPECT_TRUE(e.resources().allFree());
  EXPECT_EQ(complete(e.submit({})).reason, WorkflowReason::Stopped);
}
TEST(Workflow, EngineResolvesUncheckedCycle) {
  Simulator sim(2);
  ASSERT_EQ(sim.line, "READY");
  EngineConfig c;
  c.locking = LockMode::Unchecked;
  Engine e(c);
  sim.attach(e, 2);
  ASSERT_TRUE(e.start());
  e.resources().acquire(10, {0}, 10, 1, {});
  e.resources().acquire(20, {1}, 1, 2, {});
  Workflow a;
  a.withId(10)
      .withPriority(10)
      .needs({0, 1})
      .manualLocks()
      .then(AcquireDevices{{1}})
      .then(ReadRegisters{0, 0, 1, "x"});
  Workflow b;
  b.withId(20).withPriority(1).needs({0, 1}).manualLocks().then(
      AcquireDevices{{0}});
  auto first = e.submit(a);
  auto second = e.submit(b);
  auto ra = complete(first), rb = complete(second);
  EXPECT_EQ(ra.state, WorkflowState::Completed);
  EXPECT_EQ(rb.state, WorkflowState::Aborted);
  EXPECT_EQ(rb.reason, WorkflowReason::Deadlock);
  EXPECT_EQ(e.stats().deadlocksResolved, 1u);
  EXPECT_TRUE(e.resources().allFree());
}
TEST(Watchdog, IsolationOtherDevicesAndRecovery) {
  Simulator sim(2);
  ASSERT_EQ(sim.line, "READY");
  ASSERT_TRUE(sim.command("silence", 0));
  EngineConfig c;
  c.requestTimeout = RequestTimeout;
  c.watchdog.initialBackoff = Backoff;
  c.watchdog.maxBackoff = Backoff * 4;
  c.watchdog.probeTimeout = ProbeTimeout;
  Engine e(c);
  sim.attach(e, 2);
  ASSERT_TRUE(e.start());
  std::vector<WorkflowHandle> pending;
  for (int i = 0; i < 6; ++i) {
    Workflow w;
    w.needs({0}).then(ReadRegisters{0, 0, 1, "x"});
    pending.push_back(e.submit(w));
  }
  Workflow good;
  good.needs({1})
      .then(WriteRegister{1, 0, 55})
      .then(ReadRegisters{1, 0, 1, "x"});
  auto other = e.submit(good);
  ASSERT_TRUE(e.waitForHealth(0, Health::Faulted, Deadline));
  size_t isolated = 0;
  for (auto h : pending)
    if (complete(h).reason == WorkflowReason::DeviceFaulted)
      ++isolated;
  EXPECT_GE(isolated, 4u);
  EXPECT_EQ(complete(other).variables.at("x"), (std::vector<uint16_t>{55}));
  Workflow bad;
  bad.needs({0}).then(ReadRegisters{0, 0, 1, "x"});
  EXPECT_EQ(complete(e.submit(bad)).reason, WorkflowReason::DeviceFaulted);
  auto events = e.events().snapshot();
  std::vector<std::string> health;
  for (auto &event : events)
    if (event.type == "device_health" && event.device == 0)
      health.push_back(event.detail);
  ASSERT_GE(health.size(), 3u);
  EXPECT_EQ(health[0], "Healthy");
  EXPECT_EQ(health[1], "Suspect");
  EXPECT_EQ(health[2], "Faulted");
  ASSERT_TRUE(sim.command("resume", 0));
  ASSERT_TRUE(e.waitForHealth(0, Health::Healthy, Deadline));
  EXPECT_EQ(complete(e.submit(bad)).state, WorkflowState::Completed);
  EXPECT_TRUE(e.resources().allFree());
  EXPECT_EQ(e.stats().devicesFaulted, 1u);
}
TEST(Stress, HundredWorkflowsFortyOneDevices) {
  Simulator sim(41);
  ASSERT_EQ(sim.line, "READY");
  Engine e;
  sim.attach(e, 41);
  ASSERT_TRUE(e.start());
  std::vector<WorkflowHandle> handles;
  for (int i = 0; i < 100; ++i) {
    int a = i % 41, b = (i + 1) % 41;
    Workflow w;
    w.withPriority(i % 9)
        .needs({a, b})
        .then(WriteRegister{a, static_cast<uint16_t>(i),
                            static_cast<uint16_t>(700 + i)})
        .then(WriteRegisters{b,
                             static_cast<uint16_t>(200 + 2 * i),
                             {static_cast<uint16_t>(i), 0xbeef}})
        .then(ReadRegisters{a, static_cast<uint16_t>(i), 1, "a"})
        .then(ReadRegisters{b, static_cast<uint16_t>(200 + 2 * i), 2, "b"});
    handles.push_back(e.submit(w));
  }
  for (int i = 0; i < 100; ++i) {
    auto r = complete(handles[i]);
    ASSERT_EQ(r.state, WorkflowState::Completed) << r.detail;
    EXPECT_EQ(r.variables.at("a"),
              (std::vector<uint16_t>{static_cast<uint16_t>(700 + i)}));
    EXPECT_EQ(r.variables.at("b"),
              (std::vector<uint16_t>{static_cast<uint16_t>(i), 0xbeef}));
  }
  EXPECT_EQ(e.stats().completed, 100u);
  EXPECT_TRUE(e.resources().allFree());
}
} // namespace
