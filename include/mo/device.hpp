#pragma once
#include "mo/reactor.hpp"
#include "mo/transport.hpp"
#include <deque>
#include <memory>
namespace mo {
enum class DeviceState { Connecting, Ready, Faulted };
class Device: public std::enable_shared_from_this<Device> {
 public:
 using Callback=std::function<void(Result<Response>)>;
 Device(Reactor&,std::unique_ptr<Transport>);
 ~Device();
 void start();void stop();
 void request(Request,Callback,std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
 DeviceState state() const{return state_.load();}
 private:
 struct Pending {Request request;Callback callback;uint64_t timer;};
 struct Output {uint16_t id;Bytes bytes;size_t offset=0;};
 void begin();void enqueue(Request,Callback,std::chrono::milliseconds);
 void readable();void writable();void flush();void fail(Error);void complete(uint16_t,Result<Response>);
 Reactor& reactor_;std::unique_ptr<Transport> transport_;StreamDecoder decoder_;
 std::atomic<DeviceState> state_{DeviceState::Connecting};bool started_=false;
 std::deque<uint16_t> serial_;std::map<uint16_t,Pending> pending_;std::deque<Output> output_;uint16_t next_=0;
};
}
