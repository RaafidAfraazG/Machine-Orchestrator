#include "scenario_fixture.hpp"
namespace mo::test {
TEST(Regression, ConcurrentLog) {
  EventLog log(10000);
  std::promise<void> gate;
  auto go = gate.get_future().share();
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      go.wait();
      for (int j = 0; j < 1000; ++j)
        log.record("parallel");
    });
  gate.set_value();
  for (auto &thread : threads)
    thread.join();
  EXPECT_EQ(log.snapshot().size(), 4000u);
  EXPECT_EQ(log.dropped(), 0u);
}
TEST(Regression, ConcurrentQueue) {
  Scheduler scheduler;
  std::atomic<int> executed{0};
  std::promise<void> gate;
  auto go = gate.get_future().share();
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      go.wait();
      for (int j = 0; j < 100; ++j)
        scheduler.enqueue(j, [&] { ++executed; });
    });
  gate.set_value();
  for (auto &thread : threads)
    thread.join();
  scheduler.start();
  scheduler.stop();
  EXPECT_EQ(executed, 400);
}
TEST(Regression, ReleaseExactlyOnce) {
  EventLog log;
  ResourceManager resources(LockMode::Ordered, &log);
  resources.acquire(1, {0}, 0, 1, {});
  resources.release(1);
  resources.release(1);
  auto events = log.snapshot();
  EXPECT_EQ(std::count_if(events.begin(), events.end(),
                          [](auto &e) { return e.type == "lock_release"; }),
            1);
}
TEST(Regression, RepeatedReleasePreservesNewOwner) {
  ResourceManager resources;
  resources.acquire(1, {0}, 0, 1, {});
  resources.release(1);
  resources.acquire(2, {0}, 0, 2, {});
  resources.release(1);
  EXPECT_EQ(resources.acquire(3, {0}, 0, 3, {}).status, AcquireStatus::Blocked);
  resources.release(2);
  resources.release(3);
  EXPECT_TRUE(resources.allFree());
}
TEST(Regression, DroppedEventCount) {
  EventLog log(3);
  for (int i = 0; i < 10; ++i)
    log.record("event", i);
  EXPECT_EQ(log.dropped(), 7u);
  auto events = log.snapshot();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events.front().workflow, 7u);
}
TEST(Regression, TimeoutTargetsRequest) {
  Simulator sim;
  ASSERT_EQ(sim.ready, "READY");
  ASSERT_TRUE(sim.command("silence 0"));
  Reactor loop;
  loop.start();
  auto device = std::make_shared<Device>(
      loop, std::make_unique<TcpTransport>("127.0.0.1", sim.port));
  std::promise<Result<Response>> done;
  auto future = done.get_future();
  device->request(
      {1, 3, 0, 1, {}}, [&](auto r) { done.set_value(r); }, RequestTimeout);
  auto status = future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(status, std::future_status::ready);
  if (status == std::future_status::ready) {
    EXPECT_EQ(std::get<Error>(future.get()), Error::Timeout);
  }
  device->stop();
  std::promise<void> stopped;
  auto f = stopped.get_future();
  loop.post([&] { stopped.set_value(); });
  f.wait();
  device.reset();
  loop.stop();
  loop.join();
}
TEST(Regression, CallbackExactlyOnce) {
  Simulator sim;
  ASSERT_EQ(sim.ready, "READY");
  Reactor loop;
  loop.start();
  auto device = std::make_shared<Device>(
      loop, std::make_unique<TcpTransport>("127.0.0.1", sim.port));
  std::atomic<int> calls{0};
  std::promise<void> done;
  auto f = done.get_future();
  device->request(
      {1, 3, 0, 1, {}},
      [&](auto) {
        if (++calls == 1)
          done.set_value();
      },
      RequestTimeout);
  EXPECT_EQ(f.wait_for(ScenarioDeadline), std::future_status::ready);
  device->stop();
  std::promise<void> barrier;
  auto b = barrier.get_future();
  loop.post([&] { barrier.set_value(); });
  b.wait();
  device.reset();
  loop.stop();
  loop.join();
  EXPECT_EQ(calls, 1);
}
} // namespace mo::test
