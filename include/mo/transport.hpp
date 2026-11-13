#pragma once
#include "mo/protocol.hpp"
#include <string>
#include <sys/types.h>
namespace mo {
class Transport {
public:
  virtual ~Transport() = default;
  virtual Result<bool>
  open() = 0; // true: connected, false: connect in progress
  virtual void close() = 0;
  virtual int fd() const = 0;
  virtual Result<bool> finishConnect() = 0;
  virtual Result<size_t> send(const uint8_t *, size_t) = 0;
  virtual Result<size_t> recv(uint8_t *, size_t) = 0;
  virtual int error() const = 0;
  virtual Framing framing() const = 0;
};
class FdTransport : public Transport {
public:
  ~FdTransport() override;
  void close() override;
  int fd() const override { return fd_; }
  int error() const override { return error_; }
  Result<size_t> send(const uint8_t *, size_t) override;
  Result<size_t> recv(uint8_t *, size_t) override;

protected:
  int fd_ = -1, error_ = 0;
  bool socket_ = false;
};
class TcpTransport final : public FdTransport {
public:
  TcpTransport(std::string host, uint16_t port)
      : host_(std::move(host)), port_(port) {
    socket_ = true;
  }
  Result<bool> open() override;
  Result<bool> finishConnect() override;
  Framing framing() const override { return Framing::Tcp; }

private:
  std::string host_;
  uint16_t port_;
};
class SerialTransport final : public FdTransport {
public:
  SerialTransport(std::string path, unsigned baud = 115200)
      : path_(std::move(path)), baud_(baud) {}
  Result<bool> open() override;
  Result<bool> finishConnect() override { return true; }
  Framing framing() const override { return Framing::Rtu; }

private:
  std::string path_;
  unsigned baud_;
};
} // namespace mo
