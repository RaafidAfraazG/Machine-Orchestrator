#pragma once
#include "fault_transport.hpp"
#include "mo/engine.hpp"
#include "port_lease.hpp"
#include "timing.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace mo::test {
class Simulator {
public:
  PortLease lease;
  int port = PortLease::base, control = port - 1;
  pid_t pid = -1;
  int output = -1;
  std::string ready;
  Simulator() {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC))
      return;
    std::vector<std::string> args{MO_PYTHON,        MO_SIM,
                                  "--tcp-count",    "3",
                                  "--base-port",    std::to_string(port),
                                  "--control-port", std::to_string(control)};
    std::vector<char *> argv;
    for (auto &s : args)
      argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], 1);
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
    pollfd f{output, POLLIN, 0};
    while (poll(&f, 1, static_cast<int>(ScenarioDeadline.count() * 1000)) > 0) {
      char c;
      if (read(output, &c, 1) != 1 || c == '\n')
        break;
      ready += c;
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
  bool command(std::string command) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return false;
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(control);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
    command += '\n';
    if (ok)
      ok = send(fd, command.data(), command.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(command.size());
    char reply[3];
    if (ok)
      ok = recv(fd, reply, 3, MSG_WAITALL) == 3 &&
           std::string(reply, 3) == "OK\n";
    close(fd);
    return ok;
  }
};
class ScenarioFixture : public ::testing::Test {
protected:
  Simulator sim;
  std::array<std::shared_ptr<FaultPlan>, 3> plans{
      std::make_shared<FaultPlan>(), std::make_shared<FaultPlan>(),
      std::make_shared<FaultPlan>()};
  std::mutex auditMutex;
  std::condition_variable auditCv;
  std::vector<Event> audit;
  std::unique_ptr<Engine> engine;
  bool engineStopped = false;
  struct Submitted {
    WorkflowHandle handle;
    bool normal;
  };
  std::vector<Submitted> submitted;
  std::chrono::steady_clock::time_point deadline;
  void SetUp() override {
    ASSERT_EQ(sim.ready, "READY");
    deadline = std::chrono::steady_clock::now() + ScenarioDeadline;
  }
  void launch(EngineConfig c = {}) {
    c.requestTimeout = RequestTimeout;
    c.watchdog.probeTimeout = ProbeTimeout;
    c.watchdog.initialBackoff = InitialBackoff;
    c.watchdog.maxBackoff = MaxBackoff;
    c.transportFactory = [this](int id, const Endpoint &e) {
      return std::make_unique<FaultyTransport>(
          std::make_unique<TcpTransport>(e.host, e.port), plans[id]);
    };
    engine = std::make_unique<Engine>(c);
    engine->events().setSink([this](const Event &e) {
      std::lock_guard<std::mutex> l(auditMutex);
      audit.push_back(e);
      auditCv.notify_all();
    });
    for (int id = 0; id < 3; ++id)
      ASSERT_TRUE(engine->addDevice(
          id, {"127.0.0.1", static_cast<uint16_t>(sim.port + id), {}, 115200}));
    ASSERT_TRUE(engine->start());
    // Every scenario includes a real independent device workflow, verified
    // normal.
    auto h = send(read(2), true);
    ASSERT_EQ(result(h).state, WorkflowState::Completed);
  }
  Workflow read(int id = 0) {
    Workflow w;
    w.needs({id}).then(ReadRegisters{id, 0, 1, "value"});
    return w;
  }
  WorkflowHandle send(Workflow w, bool normal = false) {
    auto h = engine->submit(std::move(w));
    submitted.push_back({h, normal});
    return h;
  }
  std::chrono::milliseconds remaining() const {
    return std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()));
  }
  void stopEngine() {
    engine->stop();
    engineStopped = true;
  }
  WorkflowResult result(WorkflowHandle h) {
    if (h.completion.wait_until(deadline) != std::future_status::ready) {
      ADD_FAILURE() << "workflow " << h.id
                    << " missed global scenario deadline";
      stopEngine();
    }
    return h.completion.get();
  }
  bool waitEvent(std::string type, WorkflowId id = 0, std::string detail = {},
                 size_t count = 1) {
    std::unique_lock<std::mutex> l(auditMutex);
    return auditCv.wait_until(l, deadline, [&] {
      return static_cast<size_t>(
                 std::count_if(audit.begin(), audit.end(), [&](auto &e) {
                   return e.type == type && (!id || e.workflow == id) &&
                          (detail.empty() || e.detail == detail);
                 })) >= count;
    });
  }
  void bad(std::string fault) {
    ASSERT_TRUE(sim.command(fault));
    launch();
    auto h = send(read());
    EXPECT_EQ(result(h).state, WorkflowState::Failed);
  }
  void TearDown() override {
    if (!engine)
      return;
    for (auto &item : submitted) {
      auto r = result(item.handle);
      EXPECT_TRUE(r.state == WorkflowState::Completed ||
                  r.state == WorkflowState::Failed ||
                  r.state == WorkflowState::Aborted);
      if (item.normal) {
        EXPECT_EQ(r.state, WorkflowState::Completed) << r.detail;
      }
    }
    // Verify isolation again AFTER the fault timeline, unless this scenario
    // explicitly shut down the whole engine. Stop scenarios use the pre-stop
    // independent control workflow instead.
    if (!engineStopped) {
      auto control = send(read(2), true);
      EXPECT_EQ(result(control).state, WorkflowState::Completed);
    }
    stopEngine();
    EXPECT_TRUE(engine->resources().allFree());
    std::map<std::pair<WorkflowId, int>, int> locks;
    // Probe commands use workflow=command=0; pair them by device and
    // transaction.
    std::map<std::string, int> commands;
    uint64_t previous = 0;
    for (auto &e : audit) {
      EXPECT_GE(e.timestampNs, previous);
      previous = e.timestampNs;
      if (e.type == "lock_acquire")
        ++locks[{e.workflow, e.device}];
      if (e.type == "lock_release") {
        auto &n = locks[{e.workflow, e.device}];
        EXPECT_GT(n, 0);
        --n;
      }
      std::string key = std::to_string(e.device) + ":" +
                        (e.command ? std::to_string(e.command) : e.detail);
      if (e.type == "command_submitted")
        ++commands[key];
      if (e.type == "response_received" || e.type == "command_timeout" ||
          e.type == "command_failed") {
        auto &n = commands[key];
        EXPECT_GT(n, 0);
        --n;
      }
    }
    for (auto &entry : locks)
      EXPECT_EQ(entry.second, 0) << "unpaired lock " << entry.first.first;
    for (auto &entry : commands)
      EXPECT_EQ(entry.second, 0) << "unpaired command " << entry.first;
  }
};
} // namespace mo::test
