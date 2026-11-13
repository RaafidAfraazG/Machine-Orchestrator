# Protocol and simulator

[README](../README.md) · [Design](DESIGN.md) · [Verification](VERIFICATION.md) · [Latency](LATENCY.md) · [Protocol and simulator](PROTOCOL_AND_SIMULATOR.md)

## Protocol framing

The [codec](../include/mo/protocol.hpp) supports these request/response functions:

| Function | Meaning |
|---|---|
| `0x03` | Read holding registers |
| `0x06` | Write a single register |
| `0x10` | Write multiple registers |
| `function \| 0x80` | Exception response; the response carries an exception code |

TCP uses MBAP fields (transaction ID, protocol ID `0`, length, unit ID) followed
by the PDU. RTU-style serial uses unit ID + PDU + Modbus CRC16. The streaming
decoder retains arbitrary partial chunks and extracts coalesced frames. Invalid
length, unknown functions and bad CRCs are explicit `Error` values. Direct decode
can report Incomplete; `StreamDecoder::feed` retains partial data until a complete
frame is available. Malformed framing is terminal for a Device; it does not
attempt speculative byte resynchronization.

`Result<T>` holds a value or an `Error`. A Modbus exception is a valid Response
with a nonzero `exception`, rather than a transport failure. Requests and responses
use network byte order for register fields; the RTU CRC is emitted low byte first.
See [protocol.cpp](../src/protocol.cpp) and [codec tests](../tests/tests.cpp).

## Transports and devices

TCP connects without blocking, handles EINPROGRESS and enables TCP_NODELAY.
Serial uses termios raw mode and nonblocking I/O. `Error::Incomplete` from I/O
means would-block. Real endpoints use native readiness; injected delays/EAGAIN
storms use the timer-backed retry hook to avoid spinning on a ready descriptor.

Device owns its transport, decoder and outgoing queue. TCP matches transaction
IDs; serial sends in order. Timeouts include output queueing/connection time after
acceptance on the loop. A timeout with queued TCP output faults the connection
to avoid frame truncation; a fully sent TCP request may time out while the
connection remains usable. Serial timeouts fault its pending work to avoid
misidentifying a late reply. Engine watchdog policy decides isolation/recovery.

## Simulator CLI

The [simulator](../sim/device_sim.py) uses Python's standard library and asyncio,
with an in-memory register map per TCP endpoint and a PTY serial mode.

| Flag | Purpose |
|---|---|
| `--tcp-count N` | Number of simulated TCP devices |
| `--base-port P` | First device port; subsequent devices use successive ports |
| `--latency-ms X` / `--jitter-ms Y` | Response delay and random variation |
| `--serial` | Create a PTY device and print its path |
| `--control-port P` | Enable the optional loopback administration server |
| `--fault drop-connection\|delay\|garbage-response` | Static fault hook |
| `--fault-device INDEX` | Device targeted by the CLI fault hook |
| `--fault-delay-ms X` | Extra delay used by the CLI delay fault |

## Demo commands

Start a simulator in one terminal, then run the orchestrator in another:
```sh
python3 sim/device_sim.py --tcp-count 40 --base-port 15020 --control-port 15019
./build/mo_orchestrator_demo --devices 40 --base-port 15020
./build/mo_orchestrator_demo --unchecked-deadlock-demo
./build/mo_orchestrator_demo --kill-device 0 --control-port 15019
```

Restart/reset the simulator after the silence demo. The original `mo_demo` is also
available; `python3 scripts/run_demo.py` manages a temporary simulator for it.
Serial simulation: `python3 sim/device_sim.py --serial` prints its PTY path.

## Control port

The simulator's optional loopback control port accepts one newline-terminated
command per connection, replying OK or ERROR:

- `silence ID`, `resume ID`, `hold ID` (gate a response without a timing race).
- `delay ID ms`, `garbage ID`, `partial ID`, `wrong-tx ID`, `exception ID code`.
- `drop ID` closes current connections; `reset ID` clears control faults/registers and
  closes connections. CLI `--fault` flags remain available.

Commands are one line per connection and reply `OK` or `ERROR`. `resume` releases
a held response and removes silence; it does not clear delay/garbage/exception
settings. `reset` clears control faults and registers, closes existing connections
and opens held gates. Static CLI faults remain configured until simulator restart.
`drop` closes current connections while the listener remains available to reconnect.

| Command | Device-side action |
|---|---|
| `silence ID` | Discard replies, including after reconnect |
| `resume ID` | Remove silence and release a held response |
| `hold ID` | Gate the response until resume/reset; useful for deterministic timelines |
| `delay ID ms` | Add a response delay |
| `garbage ID` | Return a malformed/unknown-function reply |
| `partial ID` | Send half a response and stall until the peer disconnects |
| `wrong-tx ID` | Alter the transaction ID |
| `exception ID code` | Reply with the selected Modbus exception |
| `drop ID` | Close current client connections |
| `reset ID` | Clear control faults/registers, release gates and close clients |

The control port is a local test hook, not a device-management protocol for
production deployments. The engine and simulator can also be exercised through
[FaultyTransport](../tests/fault_transport.hpp) without simulator-side controls.
