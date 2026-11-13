#include "mo/device.hpp"
namespace mo {
Device::Device(Reactor &r, std::unique_ptr<Transport> t)
    : reactor_(r), transport_(std::move(t)), decoder_(transport_->framing()) {}
Device::~Device() {
  // Defer descriptor deregistration and closure to its owner thread.
  auto t = std::shared_ptr<Transport>(std::move(transport_));
  auto *loop = &reactor_;
  reactor_.post([loop, t, pending = std::move(pending_)]() mutable {
    loop->unwatch(t->fd());
    t->close();
    for (auto &entry : pending) {
      loop->cancel(entry.second.timer);
      entry.second.callback(Error::Closed);
    }
  });
}
void Device::start() {
  auto self = shared_from_this();
  reactor_.post([self] { self->begin(); });
}
void Device::stop() {
  auto self = shared_from_this();
  reactor_.post([self] { self->fail(Error::Closed); });
}
void Device::isolate() {
  auto self = shared_from_this();
  reactor_.post([self] {
    self->isolated_ = true;
    self->fail(Error::DeviceFaulted);
  });
}
void Device::reconnect() {
  auto self = shared_from_this();
  reactor_.post([self] {
    self->fail(Error::Closed);
    self->decoder_ = StreamDecoder(self->transport_->framing());
    self->isolated_ = false;
    self->started_ = false;
    self->state_ = DeviceState::Connecting;
    self->begin();
  });
}
void Device::setObserver(Observer observer) {
  auto self = shared_from_this();
  reactor_.post([self, observer = std::move(observer)]() mutable {
    self->observer_ = std::move(observer);
  });
}
void Device::begin() {
  if (started_)
    return;
  started_ = true;
  auto r = transport_->open();
  if (std::holds_alternative<Error>(r)) {
    fail(std::get<Error>(r));
    return;
  }
  if (std::get<bool>(r))
    state_ = DeviceState::Ready;
  std::weak_ptr<Device> w = shared_from_this();
  if (!reactor_.watch(transport_->fd(), state_ == DeviceState::Connecting,
                      {[w] {
                         if (auto s = w.lock())
                           s->readable();
                       },
                       [w] {
                         if (auto s = w.lock())
                           s->writable();
                       },
                       [w] {
                         if (auto s = w.lock())
                           s->fail(Error::Io);
                       }}))
    fail(Error::Io);
}
void Device::request(Request r, Callback cb, std::chrono::milliseconds timeout,
                     CommandContext context) {
  auto self = shared_from_this();
  reactor_.post(
      [self, r = std::move(r), cb = std::move(cb), timeout, context]() mutable {
        self->enqueue(std::move(r), std::move(cb), timeout, context);
      });
}
void Device::enqueue(Request r, Callback cb, std::chrono::milliseconds timeout,
                     CommandContext context) {
  if (!started_)
    begin();
  if (state_ == DeviceState::Faulted) {
    cb(isolated_ ? Error::DeviceFaulted : Error::Closed);
    return;
  }
  if (pending_.size() >= 65536) {
    cb(Error::Exhausted);
    return;
  }
  uint16_t id = next_++;
  while (pending_.count(id))
    id = next_++;
  auto encoded = encodeRequest(r, transport_->framing(), id);
  if (auto e = std::get_if<Error>(&encoded)) {
    cb(*e);
    return;
  }
  std::weak_ptr<Device> w = shared_from_this();
  auto timer = reactor_.after(timeout, [w, id] {
    if (auto s = w.lock()) {
      if (s->transport_->framing() == Framing::Rtu)
        s->fail(Error::Timeout);
      else { // Partial writes cannot safely be removed from the byte stream.
        bool queued = false;
        for (auto &o : s->output_)
          if (o.id == id)
            queued = true;
        if (queued)
          s->fail(Error::Timeout);
        else
          s->complete(id, Error::Timeout);
      }
    }
  });
  if (observer_)
    observer_("command_submitted", id, context);
  pending_.emplace(id, Pending{std::move(r), std::move(cb), timer, context});
  if (transport_->framing() == Framing::Rtu)
    serial_.push_back(id);
  output_.push_back(Output{id, std::get<Bytes>(std::move(encoded)), 0});
  flush();
}
void Device::writable() {
  if (state_ == DeviceState::Connecting) {
    auto r = transport_->finishConnect();
    if (auto e = std::get_if<Error>(&r)) {
      fail(*e);
      return;
    }
    state_ = DeviceState::Ready;
  }
  flush();
}
void Device::flush() {
  if (state_ != DeviceState::Ready)
    return;
  while (!output_.empty()) {
    // Only send the first serial request until its response arrives.
    if (transport_->framing() == Framing::Rtu &&
        output_.front().id != serial_.front())
      break;
    auto &o = output_.front();
    auto r =
        transport_->send(o.bytes.data() + o.offset, o.bytes.size() - o.offset);
    if (auto e = std::get_if<Error>(&r)) {
      if (*e == Error::Incomplete)
        break;
      fail(*e);
      return;
    }
    auto n = std::get<size_t>(r);
    if (!n) {
      fail(Error::Closed);
      return;
    }
    o.offset += n;
    if (o.offset == o.bytes.size()) {
      if (observer_)
        observer_("command_sent", o.id, pending_.at(o.id).context);
      output_.pop_front();
    }
  }
  bool writable = !output_.empty();
  if (transport_->framing() == Framing::Rtu && writable &&
      output_.front().id != serial_.front())
    writable = false;
  if (!reactor_.modify(transport_->fd(), writable))
    fail(Error::Io);
}
void Device::readable() {
  uint8_t buf[4096];
  for (;;) {
    auto r = transport_->recv(buf, sizeof(buf));
    if (auto e = std::get_if<Error>(&r)) {
      if (*e != Error::Incomplete)
        fail(*e);
      return;
    }
    Bytes b(buf, buf + std::get<size_t>(r));
    for (auto &decoded : decoder_.feed(b)) {
      if (auto e = std::get_if<Error>(&decoded)) {
        fail(*e);
        return;
      }
      auto response = std::get<Response>(decoded);
      if (pending_.empty())
        continue;
      auto id = transport_->framing() == Framing::Tcp ? response.transaction
                                                      : serial_.front();
      auto i = pending_.find(id);
      if (i == pending_.end())
        continue;
      auto &req = i->second.request;
      if (response.unit != req.unit || response.function != req.function ||
          (!response.exception &&
           ((req.function == 3 && response.values.size() != req.count) ||
            (req.function != 3 && response.address != req.address) ||
            (req.function == 6 && response.values != req.values) ||
            (req.function == 16 && response.count != req.count)))) {
        fail(Error::BadLength);
        return;
      }
      complete(id, response);
      flush();
      if (state_ == DeviceState::Faulted)
        return;
    }
  }
}
void Device::complete(uint16_t id, Result<Response> r) {
  auto i = pending_.find(id);
  if (i == pending_.end())
    return;
  if (observer_)
    observer_(std::holds_alternative<Error>(r) &&
                      std::get<Error>(r) == Error::Timeout
                  ? "command_timeout"
                  : "response_received",
              id, i->second.context);
  auto cb = std::move(i->second.callback);
  reactor_.cancel(i->second.timer);
  pending_.erase(i);
  if (transport_->framing() == Framing::Rtu && !serial_.empty())
    serial_.pop_front();
  cb(std::move(r));
}
void Device::fail(Error e) {
  state_ = DeviceState::Faulted;
  reactor_.unwatch(transport_->fd());
  transport_->close();
  output_.clear();
  serial_.clear();
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto &p : pending) {
    reactor_.cancel(p.second.timer);
    if (observer_)
      observer_(e == Error::Timeout ? "command_timeout" : "command_failed",
                p.first, p.second.context);
    p.second.callback(e);
  }
}
} // namespace mo
