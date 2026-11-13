#include "mo/device.hpp"
#include "port_lease.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <future>
#include <gtest/gtest.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace mo;
using namespace std::chrono_literals;
namespace {
template <class T> T value(Result<T> r) {
  EXPECT_TRUE(std::holds_alternative<T>(r));
  return std::get<T>(r);
}
TEST(Codec, FunctionsAndFraming) {
  for (auto f : {Framing::Tcp, Framing::Rtu})
    for (uint8_t fc : {3, 6, 16}) {
      Request q{7, fc, 12, 2, {}};
      if (fc == 6) {
        q.count = 1;
        q.values = {0x1234};
      }
      if (fc == 16)
        q.values = {12, 34};
      auto wire = value(encodeRequest(q, f, 42));
      EXPECT_EQ(wire[f == Framing::Tcp ? 7 : 1], fc);
      Response r{42, 7, fc, 0, 12, 2, {12, 34}};
      if (fc == 6)
        r.values = {0x1234};
      auto encoded = value(encodeResponse(r, f));
      auto decoded = value(decodeResponse(encoded, f));
      EXPECT_EQ(decoded.function, fc);
      EXPECT_EQ(decoded.unit, 7);
      EXPECT_EQ(decoded.transaction, f == Framing::Tcp ? 42 : 0);
      if (fc != 16)
        EXPECT_EQ(decoded.values, r.values);
      else
        EXPECT_EQ(decoded.count, 2);
      r.exception = 2;
      decoded = value(decodeResponse(value(encodeResponse(r, f)), f));
      EXPECT_EQ(decoded.exception, 2);
    }
}
TEST(Codec, GoldenRequestsAndCrc) {
  Bytes vector{1, 3, 0, 0, 0, 10};
  EXPECT_EQ(crc16(vector), 0xcdc5);
  EXPECT_EQ(value(encodeRequest({1, 3, 0, 10, {}}, Framing::Rtu)),
            (Bytes{1, 3, 0, 0, 0, 10, 0xc5, 0xcd}));
  EXPECT_EQ(value(encodeRequest({1, 6, 2, 1, {0x1234}}, Framing::Tcp, 9)),
            (Bytes{0, 9, 0, 0, 0, 6, 1, 6, 0, 2, 0x12, 0x34}));
  EXPECT_EQ(value(encodeRequest({1, 16, 2, 2, {1, 2}}, Framing::Rtu)),
            (Bytes{1, 16, 0, 2, 0, 2, 4, 0, 1, 0, 2, 0xa2, 0x77}));
}
TEST(Codec, StreamingAndErrors) {
  for (auto f : {Framing::Tcp, Framing::Rtu}) {
    StreamDecoder d(f);
    auto b = value(encodeResponse({12, 1, 3, 0, 0, 2, {10, 20}}, f));
    for (size_t i = 0; i + 1 < b.size(); ++i)
      EXPECT_TRUE(d.feed({b[i]}).empty());
    auto out = d.feed({b.back()});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(value(out[0]).values, (std::vector<uint16_t>{10, 20}));
    Bytes both = b;
    both.insert(both.end(), b.begin(), b.end());
    EXPECT_EQ(d.feed(both).size(), 2u);
    b.pop_back();
    EXPECT_EQ(std::get<Error>(decodeResponse(b, f)),
              f == Framing::Tcp ? Error::Incomplete : Error::BadCrc);
  }
  auto b = value(encodeResponse({0, 1, 6, 0, 0, 1, {1}}, Framing::Rtu));
  b.back() ^= 1;
  EXPECT_EQ(std::get<Error>(decodeResponse(b, Framing::Rtu)), Error::BadCrc);
  EXPECT_EQ(std::get<Error>(encodeRequest({1, 99, 0, 1, {}}, Framing::Tcp)),
            Error::UnknownFunction);
  EXPECT_EQ(std::get<Error>(encodeRequest({1, 3, 0, 0, {}}, Framing::Tcp)),
            Error::InvalidArgument);
  EXPECT_EQ(std::get<Error>(
                decodeResponse({0, 0, 0, 1, 0, 3, 1, 0x83, 2}, Framing::Tcp)),
            Error::BadProtocol);
  EXPECT_EQ(std::get<Error>(
                decodeResponse({0, 0, 0, 0, 0, 3, 1, 99, 2}, Framing::Tcp)),
            Error::UnknownFunction);
  StreamDecoder d(Framing::Tcp);
  EXPECT_EQ(std::get<Error>(d.feed({0, 0, 0, 0, 255, 255})[0]),
            Error::BadLength);
}
struct Loop {
  Reactor r;
  Loop() { r.start(); }
  ~Loop() {
    r.stop();
    r.join();
  }
  void sync() {
    std::promise<void> p;
    auto f = p.get_future();
    r.post([&] { p.set_value(); });
    f.wait();
  }
};
TEST(Reactor, PostTimerAndCancel) {
  Loop l;
  std::promise<void> p;
  auto f = p.get_future();
  std::atomic<bool> cancelled{false};
  std::thread other([&] {
    l.r.post([&] {
      auto id = l.r.after(1ms, [&] { cancelled = true; });
      l.r.cancel(id);
      l.r.after(20ms, [&] { p.set_value(); });
    });
  });
  other.join();
  EXPECT_EQ(f.wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(cancelled);
}
TEST(Reactor, RegisterUnregister) {
  Loop l;
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  ASSERT_GE(fd, 0);
  std::promise<void> p;
  auto f = p.get_future();
  std::atomic<int> calls{0};
  l.r.post([&] {
    EXPECT_TRUE(l.r.watch(fd, false,
                          {[&] {
                             uint64_t n;
                             EXPECT_EQ(read(fd, &n, 8), 8);
                             ++calls;
                             l.r.unwatch(fd);
                             p.set_value();
                           },
                           {},
                           {}}));
    uint64_t n = 1;
    EXPECT_EQ(write(fd, &n, 8), 8);
  });
  EXPECT_EQ(f.wait_for(1s), std::future_status::ready);
  l.sync();
  EXPECT_EQ(calls, 1);
  close(fd);
}
bool ready(int fd, short events) {
  pollfd p{fd, events, 0};
  return poll(&p, 1, 2000) > 0;
}
TEST(Transport, TcpLoopback) {
  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)), 0);
  socklen_t n = sizeof(a);
  ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&a), &n), 0);
  ASSERT_EQ(listen(listener, 1), 0);
  TcpTransport t("127.0.0.1", ntohs(a.sin_port));
  ASSERT_TRUE(std::holds_alternative<bool>(t.open()));
  ASSERT_TRUE(ready(t.fd(), POLLOUT));
  EXPECT_TRUE(value(t.finishConnect()));
  int peer = accept(listener, nullptr, nullptr);
  ASSERT_GE(peer, 0);
  uint8_t b = 42, r = 0;
  EXPECT_EQ(value(t.send(&b, 1)), 1u);
  ASSERT_TRUE(ready(peer, POLLIN));
  EXPECT_EQ(read(peer, &r, 1), 1);
  EXPECT_EQ(r, 42);
  EXPECT_EQ(write(peer, &b, 1), 1);
  ASSERT_TRUE(ready(t.fd(), POLLIN));
  EXPECT_EQ(value(t.recv(&r, 1)), 1u);
  EXPECT_EQ(std::get<Error>(t.recv(&r, 1)), Error::Incomplete);
  close(peer);
  close(listener);
  t.close();
  EXPECT_EQ(t.fd(), -1);
}
TEST(Transport, SerialPty) {
  int master, slave;
  char name[128];
  ASSERT_EQ(openpty(&master, &slave, name, nullptr, nullptr), 0);
  SerialTransport t(name, 9600);
  ASSERT_TRUE(value(t.open()));
  uint8_t b = 0x80, r = 0;
  EXPECT_EQ(value(t.send(&b, 1)), 1u);
  ASSERT_TRUE(ready(master, POLLIN));
  EXPECT_EQ(read(master, &r, 1), 1);
  EXPECT_EQ(r, b);
  EXPECT_EQ(write(master, &b, 1), 1);
  ASSERT_TRUE(ready(t.fd(), POLLIN));
  EXPECT_EQ(value(t.recv(&r, 1)), 1u);
  EXPECT_EQ(r, b);
  close(master);
  close(slave);
}
class Simulator {
public:
  pid_t pid = -1;
  int output = -1;
  std::string line;
  std::unique_ptr<mo::test::PortLease> lease;
  explicit Simulator(std::vector<std::string> args) {
    if (std::find(args.begin(), args.end(), "--serial") == args.end())
      lease = std::make_unique<mo::test::PortLease>();
    int pipes[2];
    if (pipe(pipes))
      return;
    pid = fork();
    if (pid == 0) {
      dup2(pipes[1], STDOUT_FILENO);
      close(pipes[0]);
      close(pipes[1]);
      std::vector<std::string> strings{MO_PYTHON, MO_SIM};
      strings.insert(strings.end(), args.begin(), args.end());
      std::vector<char *> argv;
      for (auto &s : strings)
        argv.push_back(s.data());
      argv.push_back(nullptr);
      execv(MO_PYTHON, argv.data());
      _exit(127);
    }
    close(pipes[1]);
    output = pipes[0];
    while (ready(output, POLLIN)) {
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
};
std::future<Result<Response>> ask(std::shared_ptr<Device> d, Request r,
                                  std::chrono::milliseconds timeout = 1s) {
  auto p = std::make_shared<std::promise<Result<Response>>>();
  auto f = p->get_future();
  d->request(
      std::move(r), [p](auto result) { p->set_value(std::move(result)); },
      timeout);
  return f;
}
TEST(Integration, FortyDevicesAndTimeout) {
  // Reserve a contiguous local range; report a collision as a startup failure.
  int port = mo::test::PortLease::base;
  Simulator sim({"--tcp-count", "41", "--base-port", std::to_string(port),
                 "--latency-ms", "1", "--jitter-ms", "1", "--fault", "delay",
                 "--fault-device", "40"});
  ASSERT_EQ(sim.line, "READY");
  Loop l;
  std::vector<std::shared_ptr<Device>> devices;
  std::vector<std::future<Result<Response>>> writes;
  for (int i = 0; i < 41; ++i) {
    auto d = std::make_shared<Device>(
        l.r, std::make_unique<TcpTransport>("127.0.0.1", port + i));
    devices.push_back(d);
    d->start();
    if (i < 40)
      writes.push_back(ask(d, {1,
                               16,
                               10,
                               2,
                               {static_cast<uint16_t>(100 + i),
                                static_cast<uint16_t>(200 + i)}}));
  }
  for (auto &f : writes) {
    ASSERT_EQ(f.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(value(f.get()).function, 16);
  }
  std::vector<std::future<Result<Response>>> reads;
  for (int i = 0; i < 40; ++i)
    reads.push_back(ask(devices[i], {1, 3, 10, 2, {}}));
  for (int i = 0; i < 40; ++i) {
    ASSERT_EQ(reads[i].wait_for(3s), std::future_status::ready);
    EXPECT_EQ(value(reads[i].get()).values,
              (std::vector<uint16_t>{static_cast<uint16_t>(100 + i),
                                     static_cast<uint16_t>(200 + i)}));
  }
  auto timeout = ask(devices[40], {1, 3, 0, 1, {}}, 50ms);
  ASSERT_EQ(timeout.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(std::get<Error>(timeout.get()), Error::Timeout);
  auto invalid = ask(devices[0], {1, 3, 1024, 1, {}});
  ASSERT_EQ(invalid.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(value(invalid.get()).exception, 2);
  for (auto &d : devices)
    d->stop();
  l.sync();
  devices.clear();
  l.sync();
}
TEST(Integration, SerialSimulatorAndQueue) {
  Simulator sim({"--serial"});
  ASSERT_EQ(sim.line.substr(0, 9), "/dev/pts/");
  Loop l;
  auto d = std::make_shared<Device>(
      l.r, std::make_unique<SerialTransport>(sim.line));
  d->start();
  auto w = ask(d, {1, 6, 5, 1, {1234}});
  auto r = ask(d, {1, 3, 5, 1, {}});
  ASSERT_EQ(w.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(value(w.get()).values, (std::vector<uint16_t>{1234}));
  ASSERT_EQ(r.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(value(r.get()).values, (std::vector<uint16_t>{1234}));
  d->stop();
  l.sync();
  d.reset();
  l.sync();
}
TEST(Integration, Faults) {
  for (auto fault : {"drop-connection", "garbage-response"}) {
    int port = mo::test::PortLease::base;
    Simulator sim({"--base-port", std::to_string(port), "--fault", fault});
    ASSERT_EQ(sim.line, "READY");
    Loop l;
    auto d = std::make_shared<Device>(
        l.r, std::make_unique<TcpTransport>("127.0.0.1", port));
    auto r = ask(d, {1, 3, 0, 1, {}});
    ASSERT_EQ(r.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(std::holds_alternative<Error>(r.get()));
    EXPECT_EQ(d->state(), DeviceState::Faulted);
    d->stop();
    l.sync();
    d.reset();
    l.sync();
  }
}

class ConnectedTransport : public FdTransport {
public:
  explicit ConnectedTransport(int fd) {
    fd_ = fd;
    socket_ = true;
  }
  Result<bool> open() override { return true; }
  Result<bool> finishConnect() override { return true; }
  Framing framing() const override { return Framing::Tcp; }
};
TEST(Integration, OutOfOrderTransactionMatching) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       sockets),
            0);
  Loop l;
  auto d = std::make_shared<Device>(
      l.r, std::make_unique<ConnectedTransport>(sockets[0]));
  auto first = ask(d, {1, 3, 1, 1, {}});
  auto second = ask(d, {1, 3, 2, 1, {}});
  Bytes requests(24);
  size_t offset = 0;
  while (offset < requests.size()) {
    ASSERT_TRUE(ready(sockets[1], POLLIN));
    auto n =
        read(sockets[1], requests.data() + offset, requests.size() - offset);
    ASSERT_GT(n, 0);
    offset += static_cast<size_t>(n);
  }
  auto tx = [&](size_t p) {
    return static_cast<uint16_t>((requests[p] << 8) | requests[p + 1]);
  };
  auto b = value(encodeResponse({tx(12), 1, 3, 0, 0, 1, {222}}, Framing::Tcp));
  auto a = value(encodeResponse({tx(0), 1, 3, 0, 0, 1, {111}}, Framing::Tcp));
  b.insert(b.end(), a.begin(), a.end());
  ASSERT_EQ(write(sockets[1], b.data(), b.size()),
            static_cast<ssize_t>(b.size()));
  ASSERT_EQ(first.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(second.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(value(first.get()).values, (std::vector<uint16_t>{111}));
  EXPECT_EQ(value(second.get()).values, (std::vector<uint16_t>{222}));
  d->stop();
  l.sync();
  d.reset();
  l.sync();
  close(sockets[1]);
}
TEST(Integration, LateTcpResponseAndSerialTimeout) {
  int port = mo::test::PortLease::base;
  Simulator sim({"--base-port", std::to_string(port), "--latency-ms", "100"});
  ASSERT_EQ(sim.line, "READY");
  Loop l;
  auto d = std::make_shared<Device>(
      l.r, std::make_unique<TcpTransport>("127.0.0.1", port));
  auto timed = ask(d, {1, 6, 5, 1, {777}}, 20ms);
  ASSERT_EQ(timed.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(std::get<Error>(timed.get()), Error::Timeout);
  auto r = ask(d, {1, 3, 5, 1, {}}, 1s);
  ASSERT_EQ(r.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(value(r.get()).values, (std::vector<uint16_t>{777}));
  EXPECT_EQ(d->state(), DeviceState::Ready);
  d->stop();
  l.sync();
  d.reset();
  l.sync();
  Simulator serial({"--serial", "--latency-ms", "100"});
  ASSERT_EQ(serial.line.substr(0, 9), "/dev/pts/");
  d = std::make_shared<Device>(l.r,
                               std::make_unique<SerialTransport>(serial.line));
  auto one = ask(d, {1, 3, 0, 1, {}}, 20ms);
  auto two = ask(d, {1, 3, 1, 1, {}}, 1s);
  ASSERT_EQ(one.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(two.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(std::get<Error>(one.get()), Error::Timeout);
  EXPECT_EQ(std::get<Error>(two.get()), Error::Timeout);
  EXPECT_EQ(d->state(), DeviceState::Faulted);
  d->stop();
  l.sync();
  d.reset();
  l.sync();
}
} // namespace
