#include "scenario_fixture.hpp"
#include <sstream>
namespace mo::test {
using FaultScenarios = ScenarioFixture;
TEST_F(FaultScenarios, SilentDevice) { bad("silence 0"); }
TEST_F(FaultScenarios, SlowBelowTimeout) {
  ASSERT_TRUE(sim.command("delay 0 " + std::to_string(SlowSuccess.count())));
  launch();
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, SlowAboveTimeout) {
  bad("delay 0 " + std::to_string(SlowFailure.count()));
}
TEST_F(FaultScenarios, GarbageResponse) { bad("garbage 0"); }
TEST_F(FaultScenarios, PartialFrameStall) { bad("partial 0"); }
TEST_F(FaultScenarios, WrongTransactionId) { bad("wrong-tx 0"); }
TEST_F(FaultScenarios, ExceptionIllegalAddress) {
  ASSERT_TRUE(sim.command("exception 0 2"));
  launch();
  auto r = result(send(read()));
  EXPECT_EQ(r.reason, WorkflowReason::RequestFailed);
  EXPECT_EQ(r.detail, "Modbus exception 2");
  EXPECT_EQ(engine->health(0), Health::Healthy);
}
TEST_F(FaultScenarios, ExceptionBusy) {
  ASSERT_TRUE(sim.command("exception 0 6"));
  launch();
  EXPECT_EQ(result(send(read())).detail, "Modbus exception 6");
}
TEST_F(FaultScenarios, DropMidRequest) {
  ASSERT_TRUE(sim.command("hold 0"));
  launch();
  auto h = send(read());
  ASSERT_TRUE(waitEvent("command_sent", h.id));
  ASSERT_TRUE(sim.command("drop 0"));
  EXPECT_EQ(result(h).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, DropDuringConnect) {
  plans[0]->reset = 1;
  launch();
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, FailOpen) {
  plans[0]->failOpen = 1;
  launch();
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, DeviceRecovers) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  EXPECT_EQ(result(send(read())).reason, WorkflowReason::DeviceFaulted);
  ASSERT_TRUE(sim.command("reset 0"));
  ASSERT_TRUE(engine->waitForHealth(0, Health::Healthy, remaining()));
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, DeviceFlapsTwice) {
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  for (int round = 0; round < 2; ++round) {
    ASSERT_TRUE(sim.command("silence 0"));
    EXPECT_EQ(result(send(read())).reason, WorkflowReason::DeviceFaulted);
    ASSERT_TRUE(sim.command("reset 0"));
    ASSERT_TRUE(engine->waitForHealth(0, Health::Healthy, remaining()));
    EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
  }
}
TEST_F(FaultScenarios, ClientDropSend) {
  launch();
  plans[0]->dropSends = 1;
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, ClientCorruptReceive) {
  launch();
  plans[0]->corrupt = 1;
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, ClientTruncateReceive) {
  launch();
  plans[0]->truncate = 1;
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, ClientDelayedReceiveSuccess) {
  launch();
  plans[0]->delayRecvMs = static_cast<int>(SlowSuccess.count());
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, ClientDelayedReceiveTimeout) {
  launch();
  plans[0]->delayRecvMs = static_cast<int>(SlowFailure.count());
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, ForcedResetOnSend) {
  launch();
  plans[0]->reset = 1;
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
}
TEST_F(FaultScenarios, SendEagainStorm) {
  launch();
  plans[0]->sendEagain = 20;
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, RecvEagainStorm) {
  launch();
  plans[0]->recvEagain = 20;
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, CombinedWouldBlockAndDelay) {
  launch();
  plans[0]->sendEagain = 3;
  plans[0]->recvEagain = 3;
  plans[0]->delayRecvMs = static_cast<int>(SlowSuccess.count());
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, TwoWorkflowCycle) {
  EngineConfig c;
  c.locking = LockMode::Unchecked;
  launch(c);
  engine->resources().acquire(100, {0}, 10, 1, {});
  engine->resources().acquire(200, {1}, 1, 2, {});
  Workflow a;
  a.withId(100).withPriority(10).needs({0, 1}).manualLocks().then(
      AcquireDevices{{1}});
  Workflow b;
  b.withId(200).withPriority(1).needs({0, 1}).manualLocks().then(
      AcquireDevices{{0}});
  auto x = send(a), y = send(b);
  EXPECT_EQ(result(x).state, WorkflowState::Completed);
  EXPECT_EQ(result(y).reason, WorkflowReason::Deadlock);
}
TEST_F(FaultScenarios, ThreeWorkflowCycle) {
  EngineConfig c;
  c.locking = LockMode::Unchecked;
  launch(c);
  for (int i = 0; i < 3; ++i)
    engine->resources().acquire(100 + i, {i}, 10 - i, i + 1, {});
  std::vector<WorkflowHandle> h;
  for (int i = 0; i < 3; ++i) {
    Workflow w;
    w.withId(100 + i).withPriority(10 - i).needs({0, 1, 2}).manualLocks().then(
        AcquireDevices{{(i + 1) % 3}});
    h.push_back(send(w));
  }
  EXPECT_EQ(result(h[2]).reason, WorkflowReason::Deadlock);
  EXPECT_EQ(result(h[1]).state, WorkflowState::Completed);
  EXPECT_EQ(result(h[0]).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, DeadlockAndDeviceFault) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.locking = LockMode::Unchecked;
  c.watchdog.failureThreshold = 1;
  launch(c);
  engine->resources().acquire(100, {0}, 10, 1, {});
  engine->resources().acquire(200, {1}, 1, 2, {});
  Workflow a;
  a.withId(100)
      .withPriority(10)
      .needs({0, 1})
      .manualLocks()
      .then(AcquireDevices{{1}})
      .then(ReadRegisters{0, 0, 1, "x"});
  Workflow b;
  b.withId(200).withPriority(1).needs({0, 1}).manualLocks().then(
      AcquireDevices{{0}});
  auto x = send(a), y = send(b);
  EXPECT_EQ(result(y).reason, WorkflowReason::Deadlock);
  EXPECT_EQ(result(x).reason, WorkflowReason::DeviceFaulted);
  EXPECT_EQ(engine->stats().deadlocksResolved, 1u);
}
TEST_F(FaultScenarios, LockOrderViolation) {
  launch();
  EXPECT_EQ(engine->resources().acquire(100, {1}, 0, 1, {}).status,
            AcquireStatus::Granted);
  EXPECT_EQ(engine->resources().acquire(100, {0}, 0, 1, {}).status,
            AcquireStatus::RankViolation);
  engine->resources().release(100);
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, AbortWhileBlocked) {
  launch();
  Workflow a;
  a.needs({0}).then(Delay{LongDelay});
  auto x = send(a);
  ASSERT_TRUE(waitEvent("lock_acquire", x.id));
  auto y = send(read());
  ASSERT_TRUE(waitEvent("workflow_state", y.id, "Blocked"));
  stopEngine();
  EXPECT_EQ(result(x).state, WorkflowState::Aborted);
  EXPECT_EQ(result(y).state, WorkflowState::Aborted);
}
TEST_F(FaultScenarios, AbortInFlight) {
  ASSERT_TRUE(sim.command("hold 0"));
  launch();
  auto h = send(read());
  ASSERT_TRUE(waitEvent("command_sent", h.id));
  stopEngine();
  EXPECT_EQ(result(h).state, WorkflowState::Aborted);
}
TEST_F(FaultScenarios, SubmitDuringStop) {
  launch();
  Workflow w;
  w.needs({0}).then(Delay{LongDelay});
  auto h = send(w);
  ASSERT_TRUE(waitEvent("lock_acquire", h.id));
  auto stop = std::async(std::launch::async, [this] { stopEngine(); });
  EXPECT_TRUE(waitEvent("workflow_state", h.id, "Aborted"));
  auto later = send(read());
  EXPECT_EQ(result(later).reason, WorkflowReason::Stopped);
  stop.get();
}
TEST_F(FaultScenarios, StopRunningWorkflows) {
  launch();
  std::vector<WorkflowHandle> h;
  for (int i = 0; i < 20; ++i) {
    Workflow w;
    w.needs({i % 2}).then(Delay{LongDelay});
    h.push_back(send(w));
    if (i == 0) {
      ASSERT_TRUE(waitEvent("lock_acquire", h.front().id));
    }
  }
  stopEngine();
  for (auto item : h)
    EXPECT_EQ(result(item).reason, WorkflowReason::Stopped);
}
TEST_F(FaultScenarios, SuspectThreshold) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 3;
  launch(c);
  EXPECT_EQ(result(send(read())).reason, WorkflowReason::RequestFailed);
  EXPECT_EQ(engine->health(0), Health::Suspect);
}
TEST_F(FaultScenarios, FaultedRejectsImmediately) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  result(send(read()));
  EXPECT_EQ(result(send(read())).reason, WorkflowReason::DeviceFaulted);
}
TEST_F(FaultScenarios, RecoveryBackoff) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  result(send(read()));
  ASSERT_TRUE(waitEvent("device_health", 0, "Faulted", 2));
  ASSERT_TRUE(sim.command("reset 0"));
  ASSERT_TRUE(engine->waitForHealth(0, Health::Healthy, remaining()));
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, MultipleDevicesFault) {
  ASSERT_TRUE(sim.command("silence 0"));
  ASSERT_TRUE(sim.command("silence 1"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  auto a = send(read(0)), b = send(read(1));
  EXPECT_EQ(result(a).reason, WorkflowReason::DeviceFaulted);
  EXPECT_EQ(result(b).reason, WorkflowReason::DeviceFaulted);
  EXPECT_EQ(result(send(read(2), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, FaultReleasesOtherResources) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  auto w = read();
  w.needs({0, 1});
  auto a = send(w);
  ASSERT_TRUE(waitEvent("command_sent", a.id));
  auto b = send(read(1), true);
  EXPECT_EQ(result(a).reason, WorkflowReason::DeviceFaulted);
  EXPECT_EQ(result(b).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, FaultWithQueuedWorkflows) {
  EngineConfig c;
  c.locking = LockMode::Unchecked;
  c.watchdog.failureThreshold = 1;
  plans[0]->sendEagain = 10000;
  launch(c);
  std::vector<WorkflowHandle> handles;
  for (int i = 0; i < 10; ++i) {
    auto w = read();
    w.manualLocks();
    handles.push_back(send(w));
  }
  // One baseline command plus every pending device request has reached enqueue.
  ASSERT_TRUE(waitEvent("command_submitted", 0, {}, 11));
  plans[0]->reset = 1;
  for (auto h : handles)
    EXPECT_EQ(result(h).reason, WorkflowReason::DeviceFaulted);
}
TEST_F(FaultScenarios, ProbeExceptionThenRecovery) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  result(send(read()));
  ASSERT_TRUE(sim.command("exception 0 2"));
  ASSERT_TRUE(sim.command("resume 0"));
  ASSERT_TRUE(waitEvent("device_health", 0, "Faulted", 2));
  ASSERT_TRUE(sim.command("reset 0"));
  ASSERT_TRUE(engine->waitForHealth(0, Health::Healthy, remaining()));
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, LocalRejectionDoesNotFaultDevice) {
  launch();
  for (int i = 0; i < 3; ++i) {
    Workflow w;
    w.needs({0}).then(WriteRegisters{0, 0, {}});
    EXPECT_EQ(result(send(w)).reason, WorkflowReason::RequestFailed);
  }
  EXPECT_EQ(engine->health(0), Health::Healthy);
}
TEST_F(FaultScenarios, PriorityInversionAttempt) {
  ASSERT_TRUE(sim.command("hold 0"));
  launch();
  auto low = read();
  low.withPriority(-100);
  auto a = send(low, true);
  ASSERT_TRUE(waitEvent("command_sent", a.id));
  auto high = read();
  high.withPriority(100);
  auto b = send(high, true);
  ASSERT_TRUE(waitEvent("workflow_state", b.id, "Blocked"));
  EXPECT_EQ(result(send(read(1), true)).state, WorkflowState::Completed);
  ASSERT_TRUE(sim.command("resume 0"));
  EXPECT_EQ(result(a).state, WorkflowState::Completed);
  EXPECT_EQ(result(b).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, AgingUnderNewHighPriorityLoad) {
  launch();
  Scheduler::Config c;
  c.workers = 1;
  c.agingInterval = 10ms;
  std::atomic<int> time{0};
  Scheduler s(c, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::milliseconds(time.load());
  });
  std::vector<int> order;
  s.enqueue(-10, [&] { order.push_back(-1); });
  time = 10000;
  for (int i = 0; i < 100; ++i)
    s.enqueue(100, [&, i] { order.push_back(i); });
  s.start();
  s.stop();
  ASSERT_EQ(order.size(), 101u);
  EXPECT_EQ(order.front(), -1);
  send(read(), true);
}
TEST_F(FaultScenarios, FifoPressure) {
  launch();
  Scheduler::Config c;
  c.workers = 1;
  Scheduler s(c);
  std::vector<int> order;
  for (int i = 0; i < 100; ++i)
    s.enqueue(5, [&, i] { order.push_back(i); });
  s.start();
  s.stop();
  ASSERT_EQ(order.size(), 100u);
  for (int i = 0; i < 100; ++i)
    EXPECT_EQ(order[i], i);
  send(read(), true);
}
TEST_F(FaultScenarios, RealtimeUnavailableFallback) {
  EngineConfig c;
  c.scheduler.realtime = {true, 0};
  launch(c);
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
  ASSERT_TRUE(waitEvent("realtime_reactor", 0, "fallback"));
}
TEST_F(FaultScenarios, FloodFiveHundredWithFault) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  std::vector<WorkflowHandle> h;
  for (int i = 0; i < 500; ++i)
    h.push_back(send(read()));
  auto good = send(read(1), true);
  for (auto item : h)
    EXPECT_EQ(result(item).reason, WorkflowReason::DeviceFaulted);
  EXPECT_EQ(result(good).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, EventLogOverflow) {
  EngineConfig c;
  c.eventCapacity = 8;
  launch(c);
  for (int i = 0; i < 20; ++i)
    EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
  EXPECT_GT(engine->events().dropped(), 0u);
  EXPECT_EQ(engine->events().snapshot().size(), 8u);
  std::ostringstream csv;
  engine->events().dumpCsv(csv);
  EXPECT_NE(csv.str().find("# dropped_events="), std::string::npos);
}
TEST_F(FaultScenarios, VeryLongWorkflow) {
  launch();
  auto w = read();
  for (int i = 0; i < 1000; ++i)
    w.then(Conditional{"value", 0, Comparison::Equal, 0, Branch::Continue,
                       Branch::Fail, 0});
  EXPECT_EQ(result(send(w, true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, ZeroStepWorkflow) {
  launch();
  Workflow w;
  w.needs({0});
  EXPECT_EQ(result(send(w, true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, DuplicateDeclaredDevices) {
  launch();
  auto w = read();
  w.needs({0, 0, 1, 1, 0});
  EXPECT_EQ(result(send(w, true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, LateReplyAfterTimeout) {
  ASSERT_TRUE(sim.command("delay 0 " + std::to_string(SlowFailure.count())));
  launch();
  EXPECT_EQ(result(send(read())).state, WorkflowState::Failed);
  ASSERT_TRUE(waitEvent("unmatched_response"));
  ASSERT_TRUE(sim.command("delay 0 0"));
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, ReconnectOpenFailures) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  result(send(read()));
  plans[0]->failOpen = 2;
  ASSERT_TRUE(waitEvent("device_health", 0, "Faulted", 3));
  ASSERT_TRUE(sim.command("reset 0"));
  ASSERT_TRUE(engine->waitForHealth(0, Health::Healthy, remaining()));
  EXPECT_EQ(result(send(read(), true)).state, WorkflowState::Completed);
}
TEST_F(FaultScenarios, ConcurrentSubmissionsWithFault) {
  ASSERT_TRUE(sim.command("silence 0"));
  EngineConfig c;
  c.watchdog.failureThreshold = 1;
  launch(c);
  std::mutex mutex;
  std::vector<Submitted> items;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (int i = 0; i < 20; ++i) {
        int id = (i + t) % 2;
        auto h = engine->submit(read(id));
        std::lock_guard<std::mutex> l(mutex);
        items.push_back({h, id == 1});
      }
    });
  for (auto &t : threads)
    t.join();
  submitted.insert(submitted.end(), items.begin(), items.end());
  for (auto &item : items) {
    auto r = result(item.handle);
    if (item.normal)
      EXPECT_EQ(r.state, WorkflowState::Completed);
    else
      EXPECT_EQ(r.reason, WorkflowReason::DeviceFaulted);
  }
}
} // namespace mo::test
