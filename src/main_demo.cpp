#include "mo/device.hpp"
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
int main(int argc, char **argv) {
  int count = 1, port = 15020;
  try {
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc)
        return 2;
      std::string a = argv[i];
      if (a == "--devices")
        count = std::stoi(argv[i + 1]);
      else if (a == "--base-port")
        port = std::stoi(argv[i + 1]);
      else
        return 2;
    }
  } catch (...) {
    return 2;
  }
  if (count < 1 || port < 1 || port > 65535 || count > 65536 - port)
    return 2;
  mo::Reactor loop;
  if (!loop.valid())
    return 1;
  loop.start();
  std::mutex mutex;
  std::condition_variable cv;
  int remaining = count;
  bool failed = false;
  std::vector<std::shared_ptr<mo::Device>> devices;
  for (int i = 0; i < count; ++i) {
    auto d = std::make_shared<mo::Device>(
        loop, std::make_unique<mo::TcpTransport>(
                  "127.0.0.1", static_cast<uint16_t>(port + i)));
    devices.push_back(d);
    d->start();
    d->request(
        {1, 6, 10, 1, {static_cast<uint16_t>(i + 100)}},
        [&, d, i](mo::Result<mo::Response> w) {
          bool wrote = std::holds_alternative<mo::Response>(w) &&
                       !std::get<mo::Response>(w).exception;
          d->request({1, 3, 10, 1, {}}, [&, i,
                                         wrote](mo::Result<mo::Response> r) {
            bool ok = wrote && std::holds_alternative<mo::Response>(r) &&
                      std::get<mo::Response>(r).values ==
                          std::vector<uint16_t>{static_cast<uint16_t>(i + 100)};
            std::lock_guard<std::mutex> lock(mutex);
            std::cout << "Device " << i << ": "
                      << (ok ? "read-after-write OK" : "FAILED") << '\n';
            failed |= !ok;
            --remaining;
            cv.notify_one();
          });
        });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(10),
                     [&] { return remaining == 0; }))
      failed = true;
  }
  for (auto &d : devices)
    d->stop();
  std::promise<void> stopped;
  auto done = stopped.get_future();
  loop.post([&] { stopped.set_value(); });
  done.wait();
  devices.clear();
  loop.stop();
  loop.join();
  return failed ? 1 : 0;
}
