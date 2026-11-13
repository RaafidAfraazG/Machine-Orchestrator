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

Prerequisites: GCC/Clang with C++17, CMake >= 3.16, Python 3 and Linux POSIX headers.
In this cloud image, first run `source scripts/dev-env.sh` to activate the locally
installed CMake. GoogleTest 1.15.2 is fetched at configure time with a pinned SHA256. Optional sanitizers: `ENABLE_ASAN`,
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

For an automatically managed simulator/demo run:

```sh
python3 scripts/run_demo.py --devices 40
```

# API and limitations

`Result<T>` holds either a value or an `Error`; protocol exceptions are valid
`Response` values with a nonzero `exception`. `Error::Incomplete` from transport
I/O means would-block. Decoder `feed()` retains partial frames without emitting
an error; direct `decodeResponse()` reports incomplete input. Malformed framing
is terminal for a Device; no speculative byte resynchronization is attempted.

Construct devices using `std::make_shared<Device>`. `start()`, `request()` and
`stop()` post work safely from any thread. `state()` is atomic. A request starts
the device if needed. Timeouts include queueing and connecting time after the
request reaches the loop. Callbacks must not throw or block. A TCP timeout leaves
a fully sent connection usable; a timeout while output remains queued faults it
to avoid truncating frames. Serial timeouts fault all pending work. `stop()` is
terminal, closes the transport, and completes pending requests with `Closed`.

Shutdown order: post `stop()` for each device, then post and await a promise as a
barrier, release the devices while the loop still runs, and finally stop/join the
reactor. The reactor must outlive every device. Reactor registration and timer
methods run only on the loop thread (or before start); use `post()` externally.
Lifecycle `start/stop/join` control belongs to one external owner; callbacks may
call `stop()` but must not join their own thread. Stop external producers before stopping; shutdown drains already-posted work.

TCP currently takes numeric IPv4 addresses. Serial uses raw 8N1 at 9600, 19200,
38400, 57600 or 115200 baud; it implements CRC framing without enforcing RTU
silent-interval timing. No reconnect, broadcast support, queue admission policy,
workflow scheduling, deadlock detection or watchdog is provided. Transaction IDs
wrap at 16 bits; peers must not retain replies across an entire ID reuse cycle.
The simulator binds to loopback and its fault options target one device at a time.

# Phase 2 hooks

Submit operations with `Device::request(Request, callback, timeout)`. Callbacks
and atomic `Device::state()` expose completion and health for a higher-level
scheduler. `Reactor::post`, `after`, and `cancel` provide dispatch and timers;
future workflow and recovery policies can use these without per-device threads.
