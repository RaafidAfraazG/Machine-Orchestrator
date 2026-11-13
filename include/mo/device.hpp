#pragma once
#include "mo/reactor.hpp"
#include "mo/transport.hpp"
#include <deque>
#include <memory>
namespace mo {
enum class DeviceState { Connecting, Ready, Faulted };
struct CommandContext {
  uint64_t workflow = 0, command = 0;
};
class Device : public std::enable_shared_from_this<Device> {
public:
  using Callback = std::function<void(Result<Response>)>;
  Device(Reactor &, std::unique_ptr<Transport>);
  ~Device();
  // Thread-safe entry points. Callbacks execute once on the reactor thread.
  // Callbacks must not throw or block. Timeout starts when work reaches the
  // loop.
  void start();
  // Terminal shutdown: pending callbacks receive Closed; no automatic recovery.
  void stop();
  // Thread-safe; Engine invokes these from the reactor. Isolation fails queued
  // and in-flight operations. Reconnect resets framing and reopens the
  // transport.
  void isolate();
  void reconnect();
  using Observer = std::function<void(const char *, uint16_t, CommandContext)>;
  void setObserver(Observer observer);
  void
  request(Request, Callback,
          std::chrono::milliseconds timeout = std::chrono::milliseconds(1000),
          CommandContext context = {});
  DeviceState state() const { return state_.load(); }

private:
  struct Pending {
    Request request;
    Callback callback;
    uint64_t timer;
    CommandContext context;
  };
  struct Output {
    uint16_t id;
    Bytes bytes;
    size_t offset = 0;
  };
  void begin();
  bool watch();
  void suspendIo();
  void enqueue(Request, Callback, std::chrono::milliseconds, CommandContext);
  void readable();
  void writable();
  void flush();
  void fail(Error);
  void complete(uint16_t, Result<Response>);
  Reactor &reactor_;
  std::unique_ptr<Transport> transport_;
  StreamDecoder decoder_;
  std::atomic<DeviceState> state_{DeviceState::Connecting};
  bool started_ = false;
  bool isolated_ = false;
  Observer observer_;
  std::deque<uint16_t> serial_;
  std::map<uint16_t, Pending> pending_;
  std::deque<Output> output_;
  uint16_t next_ = 0;
  uint64_t ioTimer_ = 0;
};
} // namespace mo
