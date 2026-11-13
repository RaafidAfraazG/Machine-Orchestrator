#pragma once
#include "mo/transport.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
namespace mo::test {
struct FaultPlan {
  std::atomic<int> dropSends{0}, truncate{0}, corrupt{0}, wrongTransaction{0},
      reset{0}, failOpen{0}, sendEagain{0}, recvEagain{0}, delayRecvMs{0};
  static bool take(std::atomic<int> &n) {
    int old = n.load();
    while (old > 0) {
      if (n.compare_exchange_weak(old, old - 1))
        return true;
    }
    return false;
  }
};
// All I/O is loop-confined; only FaultPlan is mutated externally. Artificial
// would-block uses retryAfter() so a ready fd cannot spin the reactor.
class FaultyTransport final : public Transport {
public:
  FaultyTransport(std::unique_ptr<Transport> inner,
                  std::shared_ptr<FaultPlan> plan)
      : inner_(std::move(inner)), plan_(std::move(plan)) {}
  Result<bool> open() override {
    return FaultPlan::take(plan_->failOpen) ? Result<bool>(Error::Io)
                                            : inner_->open();
  }
  void close() override { inner_->close(); }
  int fd() const override { return inner_->fd(); }
  Result<bool> finishConnect() override {
    if (FaultPlan::take(plan_->reset))
      return Error::Io;
    return inner_->finishConnect();
  }
  int error() const override { return inner_->error(); }
  Framing framing() const override { return inner_->framing(); }
  std::chrono::milliseconds retryAfter() const override { return retry_; }
  Result<size_t> send(const uint8_t *bytes, size_t size) override {
    retry_ = std::chrono::milliseconds(0);
    if (FaultPlan::take(plan_->reset))
      return Error::Io;
    if (FaultPlan::take(plan_->sendEagain)) {
      retry_ = std::chrono::milliseconds(1);
      return Error::Incomplete;
    }
    if (FaultPlan::take(plan_->dropSends))
      return size;
    return inner_->send(bytes, size);
  }
  Result<size_t> recv(uint8_t *bytes, size_t size) override {
    using namespace std::chrono;
    retry_ = milliseconds(0);
    if (FaultPlan::take(plan_->reset))
      return Error::Io;
    auto delay = plan_->delayRecvMs.exchange(0);
    if (delay > 0)
      until_ = steady_clock::now() + milliseconds(delay);
    if (steady_clock::now() < until_) {
      retry_ = duration_cast<milliseconds>(until_ - steady_clock::now()) +
               milliseconds(1);
      return Error::Incomplete;
    }
    if (FaultPlan::take(plan_->recvEagain)) {
      retry_ = milliseconds(1);
      return Error::Incomplete;
    }
    auto result = inner_->recv(bytes, size);
    if (auto n = std::get_if<size_t>(&result)) {
      if (*n && FaultPlan::take(plan_->corrupt))
        bytes[std::min<size_t>(*n - 1, 7)] ^= 0x7f;
      if (*n > 1 && FaultPlan::take(plan_->wrongTransaction))
        bytes[0] ^= 0xff;
      if (*n > 1 && FaultPlan::take(plan_->truncate))
        *n /= 2;
    }
    return result;
  }

private:
  std::unique_ptr<Transport> inner_;
  std::shared_ptr<FaultPlan> plan_;
  std::chrono::milliseconds retry_{0};
  std::chrono::steady_clock::time_point until_{};
};
} // namespace mo::test
