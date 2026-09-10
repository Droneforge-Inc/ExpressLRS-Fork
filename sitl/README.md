# ExpressLRS protocol SITL

A small Linux host proof built from this fork's **production ELRS protocol sources**.
Independent TX and RX processes exchange real ELRS OTA packets through a coordinator.
The RX runs the production `SerialCRSF` serializer and exports timestamped CRSF UART
bytes. A read-only probe verifies them with the existing Betaflight fork's actual C
receiver parser. An optional downlink mode now accepts live FC telemetry from
DFSim's observer and delivers reassembled frames to the separate TX process.

**Scope: protocol SITL, not a complete ESP32/STM32 firmware emulator.** This build
replaces the embedded TX/RX application loops with a deliberately small host loop.
It proves that the reusable ELRS packet path runs natively and can produce bytes
accepted by Betaflight. It does not yet prove complete radio-link or latency fidelity.

## Build and run

Requires Linux, a C++17 compiler, CMake >=3.20, and Python >=3.11. Ninja is optional.
No PlatformIO, Arduino SDK, MATLAB, external Python package, download, or hardware
is needed for the native build.

From the repository root:

```sh
cmake -S sitl -B build/sitl -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sitl -j4
ctest --test-dir build/sitl --output-on-failure
python3 sitl/demo.py --bin-dir build/sitl --output build/sitl/demo
```

To enable the additional Betaflight parser compatibility check, configure the
checkout path explicitly (this compiles its parser without modifying its files):

```sh
cmake -S sitl -B build/sitl -DBETAFLIGHT_SOURCE=/path/to/betaflight-fork
cmake --build build/sitl -j4
ctest --test-dir build/sitl --output-on-failure
python3 sitl/demo.py
```

Without this option, the ELRS and IPC tests still run. The Betaflight parser test
is marked skipped, and the demo manifest records `betaflight_parser_verified: false`.

Sanitizer build:

```sh
cmake -S sitl -B build/sitl-asan -DSITL_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sitl-asan -j4
ctest --test-dir build/sitl-asan --output-on-failure
```

The demo launches and closes both processes automatically. It runs 3 seconds of
virtual time at 250 Hz, writes 750 OTA/CRSF frames and 19,500 UART bytes, and saves:

- `manifest.json`: source revision, timing assumptions, checks, limitations and hash.
- `radio.csv`: RF slot, frequency register, OTA packet, emitted CRSF frame.
- `uart.csv`: individual bytes with completion timestamps in microseconds.
- `channels.csv`: decoded raw CRSF channel values, with timestamps.
- `receiver.crsf`: concatenated ordinary CRSF frames, with no IPC envelope.
- `betaflight-decoded.txt`: the real Betaflight parser's results, when enabled.

The demo uses the previous throttle/roll sequence as a test stimulus. It performs
no flight simulation and cannot command physical hardware.

## Modules and what really executes

| Module | Responsibility |
|---|---|
| `firmware.cpp` | Small host loop; configures the production ELRS code, controls virtual RF slots and UART serialization |
| `ipc.cpp`, `include/ipc.h` | Versioned bounded framing, explicit byte order, complete reads/writes and message deadline |
| `main.cpp` | Private Unix socket, process ownership, sequence checks and duplicate-request cache |
| `client.py` | Reusable synchronous coordinator API with process cleanup |
| `demo.py` | Reproducible sample scenario and artifact export |
| `tests/betaflight_probe.c` | Calls Betaflight's unmodified `crsfDataReceive` and `crsfFrameStatus` on each timed byte |

Compiled directly from existing firmware source, with `TARGET_TX` or `TARGET_RX`
set independently and **without `UNIT_TEST`**:

- `OTA.cpp`: real channel encoding, switch multiplexing, decoding and CRC checks.
- `crc.cpp`: actual CRC implementations.
- `FHSS.cpp`, `random.cpp`: real UID-seeded frequency-hop sequence.
- `common.cpp`: this fork's radio-rate tables, time-on-air values and UID seed logic.
- `CRSF.cpp`: production CRSF helpers.
- RX additionally: `SerialCRSF.cpp`, `SerialIO.cpp`, `telemetry.cpp`.
- Downlink: RX `stubborn_sender.cpp`, TX `stubborn_receiver.cpp`.

## Optional telemetry downlink

After Configure, EnableDownlink selects an explicit 1:N experiment (empty
payload defaults to 1:2; optional uint8 N supports 2/4/8/16/32/64/128).
Every Nth slot is reserved for telemetry data. Starting at nonce 1 keeps
telemetry slots aligned with production HybridWide ACK encoding. It currently
supports 8-byte OTA only. RX
QueueTelemetry runs the real telemetry parser/queue; TransmitTelemetry fragments
its messages with StubbornSender. TX ReceiveTelemetry validates OTA and
reassembles with StubbornReceiver. Subsequent ordinary RC packets carry its real
acknowledgement bit back to RX according to the selected ratio/switch encoding.
This is not the full embedded downlink scheduler:
link-statistics/sync slots, acquisition and radio hardware remain unmodeled.

The parent DFSim repository provides `tools/build_elrs_sitl.py`,
`tools/run_elrs_telemetry.py`, and MATLAB `run_dfsim_elrs_telemetry` for a live
Betaflight telemetry observer. See `matlab/ELRS_TELEMETRY.md` there. The original
RC-only `demo.py` and Configure behavior remain supported unchanged.

Only two existing source files need host guards: `native.h` gains a simulation clock
and stream helpers; `common.cpp` omits constructing physical SX1280 hardware when
`TARGET_SITL` is set. Existing embedded build paths are preserved. Other source is
under `sitl/`; the host does not compile a fake copy of the ELRS codec.

## Timing model

The coordinator supplies monotonically increasing integer microseconds. No firmware
clock reads host wall time. TX requests must be on the selected rate's slot grid.
RX rejects delivery before the configured source table's packet airtime has elapsed.
A packet carries its slot and SX1280 frequency register as **simulation metadata**;
the RX derives the expected nonce/frequency from that shared slot clock and checks
the real OTA CRC. Packet loss means omitting a delivery. A corrupt/wrong-frequency
packet produces no new CRSF bytes. Last decoded channels remain held.

Successful reception invokes the real RX CRSF serializer. Its bytes are queued as
8N1 UART completion events: byte `n` completes at `start + ceil(n*10e6/baud)` us.
At 420,000 baud a 26-byte RC frame takes 620 us. `Advance` drains only events due
by the requested time; later events remain queued. Queues have bounded capacity
and explicit backpressure rather than silently dropping bytes.

This proof uses **pre-synchronized, RC-only radio slots** and telemetry denominator
1. It intentionally does not apply the rate table's suggested telemetry ratio.
For example, selecting the fork's custom 500 Hz DF rate uses its codec/rate/airtime,
but does not reproduce that mode's normal 1:2 downlink schedule. Default 250 Hz has
3,300 us airtime + 620 us UART serialization; 3,920 us is a modeled path delay,
**not a measured end-to-end ELRS latency**.

Full-resolution 16-channel mode alternates banks of 8 channels; each bank updates
on alternate transmitted packets. In other modes the real RX serializer puts
synthetic fixed link quality/RSSI on channels 15/16. RSSI and LQ here are fixture
values, not simulated radio measurements.

## Not implemented in this milestone

- The full `tx_main.cpp` / `rx_main.cpp` loops, ESP32/STM32 CPU/peripheral execution,
  ISR scheduling, device setup, flash/configuration lifecycle or Wi-Fi.
- Binding, on-air sync acquisition/resynchronization, model-match negotiation,
  clock drift, scanning, dynamic power, actual receiver timeout/failsafe logic.
- LoRa/FLRC waveforms, real RF, collisions, calibrated losses or SPI pin emulation.
- A complete embedded bidirectional RF telemetry scheduler, including sync and
  link-statistics slots. The optional experiment reserves data downlink slots.
  `TelemetryIn` does exercise the real FC telemetry parser/queue, but returns the
  parsed payload to the coordinator; it does not transmit it back through the air.
- TX handset UART decoding (TX input is already raw CRSF channel values).
- DVDA packet repetition modes. Unsupported configurations fail explicitly.
- Radio-derived RC control of a live Betaflight SITL. The parent simulator's
  new live integration is telemetry observation, with its existing RC path retained.

## Future Betaflight connection

Keep IPC framing separate from the UART bytes. A later coordinator can pass each
`(completion_time_us, byte)` to a Betaflight virtual UART receive callback at that
simulation time, then advance its scheduler. Enable the actual CRSF receiver path
in Betaflight SITL; do not convert these bytes back to the old direct channel-input
path, which would bypass the parser. Ordinary TCP arrival time must not become the
simulation clock. FC telemetry can travel back through `TelemetryIn`; a complete
ELRS downlink scheduler remains future work.

The process boundary is independent of the current host firmware loop, so a later
port of the full ELRS application can replace that loop behind the same protocol.
See [PROTOCOL.md](PROTOCOL.md) for the IPC contract.

Recorded local results and scope: [VERIFICATION.md](VERIFICATION.md).
