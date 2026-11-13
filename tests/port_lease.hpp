#pragma once
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>
namespace mo::test {
// All simulator fixtures, even from separate sanitizer/build processes, share a
// lease. Fixed ports below Linux's ephemeral range avoid outbound client ports.
class PortLease {
public:
  static constexpr int base = 21000;
  PortLease() {
    fd_ = ::open("/tmp/machine-orchestrator-test-ports.lock",
                 O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0)
      throw std::runtime_error("port lease open failed");
    int result;
    do {
      result = flock(fd_, LOCK_EX);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      ::close(fd_);
      throw std::runtime_error("port lease lock failed");
    }
  }
  ~PortLease() {
    flock(fd_, LOCK_UN);
    ::close(fd_);
  }
  PortLease(const PortLease &) = delete;
  PortLease &operator=(const PortLease &) = delete;

private:
  int fd_ = -1;
};
} // namespace mo::test
