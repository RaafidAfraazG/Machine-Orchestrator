# DESIGN

MachineOrchestrator is a Linux C++17 foundation. `StreamDecoder` owns framing buffers;
`TcpTransport` and `SerialTransport` own nonblocking file descriptors. `Reactor`
owns one event-loop thread, an eventfd work queue, and timed tasks. Level-triggered
epoll keeps partial I/O straightforward without requiring every callback to drain
an fd. `Device` owns its transport, decoder, transmit queue and pending requests;
all mutable device I/O state lives on the reactor thread. TCP requests are matched
by transaction ID; serial requests are serialized. Devices must be shared-owned,
and the reactor must outlive them. Callbacks run on the reactor thread and must
not block. Stop devices before stopping and joining the reactor.

Phase 2 can submit requests through `Device::request` and marshal work through
`Reactor::post`; scheduling and recovery policy belong above these interfaces.
No workflows, priorities, deadlock detection or watchdogs are included.

# Build and test

```sh
cmake -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

GoogleTest is fetched at configure time. Optional sanitizers: `ENABLE_ASAN`,
`ENABLE_UBSAN`, `ENABLE_TSAN`; ASAN and TSAN cannot be combined.

```sh
python3 sim/device_sim.py --tcp-count 40 --base-port 15020
./build/mo_demo --devices 40 --base-port 15020
python3 sim/device_sim.py --serial
```

Simulator faults: `--fault drop-connection|delay|garbage-response`, selected by
`--fault-device INDEX` (zero based); `--fault-delay-ms` controls extra delay.
The register map has 1024 zero-initialized words. Serial supports one outstanding
request; a timeout faults the device to avoid attributing a late reply to new work.
