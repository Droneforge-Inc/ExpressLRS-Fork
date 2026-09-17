# ELRS SITL IPC v1

Unix-domain `SOCK_STREAM`, one coordinator connection per process. Launch
`elrs_sitl_tx --socket /private/directory/tx.sock` or the equivalent RX executable.
The service announces `READY <path>` on stdout after binding. The socket is mode
0600. Use a private directory; no existing path is unlinked at startup. There is
no network listener, discovery, MATLAB dependency or physical serial port access.

All integers are **unsigned big-endian**, explicitly encoded; there are no native
C structure images on this wire. OTA and CRSF payload bytes retain their original
firmware format, including their own bit packing and checksums.

## Header (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic `0x4553494c` (`ESIL`) |
| 4 | 2 | Version `1` |
| 6 | 2 | Operation; response has bit `0x8000` set |
| 8 | 4 | Payload length, maximum 4096 bytes |
| 12 | 4 | Sequence, starts at 1, increments once per new request |
| 16 | 8 | Virtual time, integer microseconds, at most `2^63-1` |

Each response payload starts with a 4-byte status: zero for success, one for a
rejected operation. On rejection, the remainder is a UTF-8 reason. A response
copies the request's operation, sequence and timestamp. An error timestamp is an
echo, not a clock advancement. No partial success is returned.

One request may be outstanding at a time. The latest request/response is cached.
An identical retry with the same sequence returns exactly the cached response,
without a second transmit, receive or UART drain. This includes error responses.
Reusing a sequence with different contents, an out-of-order sequence, wrong magic,
unsupported version, oversized body or truncated message closes the connection.
Restart before sequence exhaustion; wraparound is not allowed.

Header and payload share a five-second wall-time read deadline; writes also have
a five-second socket timeout. These are liveness deadlines, not simulated delays.
Idle sessions must issue a request within the deadline. EOF exits and cleans up the
owned socket; QUIT, SIGINT and SIGTERM also close the process. A forced SIGKILL can
leave a socket path, so use a fresh private directory after abnormal termination.
No reconnect-and-resume is attempted: a fresh process has fresh protocol state.

## Operations

| ID | Operation | Request body | Successful response after status |
|---:|---|---|---|
| 1 | Hello | Empty | UTF-8 role, base revision and scope description |
| 2 | Configure | UID[6], rate index u8, switch mode u8, UART baud u32 | RF interval u32, airtime u32, OTA length u8, hop interval u8 |
| 3 | Transmit (TX) | 16 raw CRSF channels, u16 each, range 0..1984 | OTA envelope described below |
| 4 | Receive (RX) | OTA envelope | One byte: 1 accepted, 0 failed CRC/frequency/type; no UART bytes in this response |
| 5 | Advance (RX) | Empty | Count u16, followed by `count` records: timestamp u64, byte u8 |
| 6 | TelemetryIn (RX) | One complete CRSF frame (4..64 bytes) | Payload dequeued by real ELRS telemetry parser, or empty |
| 7 | Quit | Empty | Empty, followed by process exit |
| 8 | EnableDownlink | Optional uint8 denominator N (empty = 2), once after Configure at time zero | Empty; N in 2/4/8/16/32/64/128; 8-byte OTA only |
| 9 | QueueTelemetry (RX) | Complete CRC-valid CRSF frame | Empty; feeds production telemetry queue without draining it |
| 10 | TransmitTelemetry (RX) | Empty, reserved RF slot time | OTA envelope, or empty if no queued message |
| 11 | ReceiveTelemetry (TX) | OTA envelope from RX | Acceptance byte followed by a complete CRSF frame if reassembly finished; DF3 session/health frames are consumed internally in v2 mode |
| 12 | EnableUplink | Empty for historical reliable-D5 comparison, or one byte `02` for production DF3 v2 | Empty; once after EnableDownlink at time zero, before slots |
| 13 | QueueUplink (TX) | Complete CRC-valid extended CRSF frame addressed to FC or RX | Empty; D5 additionally requires a valid reference payload in v2 mode |
| 14 | ReferenceStatus | Empty | State u8, complete/ready u8, session u32, epoch u16; TX appends the 15-byte production E5 CRSF status frame |
| 15 | ReferenceReset | Empty | Empty; resets the reference session and pending reference state only |

Configure exactly once at time zero, before other data operations. Default example:
rate index 6 (250 Hz), mode 0 (HybridWide for 8-byte OTA), baud 420000. Rate indices
come from this fork's `common.cpp` SX1280 table; they are not generic ELRS enum IDs.
Modes: 0=Wide/8ch, 1=Hybrid/16ch, 2=12ch (13-byte OTA only). DVDA rates are rejected.
A full radio reset means starting a new process, which also resets codec static
state. ReferenceReset is a narrower fault-injection operation described below.

OTA envelope: TX slot index u64, SX1280 frequency register u32, OTA length u8, then
8 or 13 bytes from `OtaGeneratePacketCrc`. The coordinator transports the envelope
unchanged or intentionally corrupts/drops it to test the receiver. RX requires
strictly increasing delivered slot indices and receipt at or after
`slot * interval + airtime`. The metadata supplies pre-synchronization; it is not
an implementation of on-air sync packets or radio hardware.

Advance returns at most 400 due bytes per call (to stay below the payload limit).
Repeat at the same timestamp to drain remaining due bytes. With an undrained
queue over 4000 bytes, Receive returns backpressure **before consuming the slot**;
drain with Advance and submit the delivery again using a new request sequence.
Identical retransmission of the old request returns its cached backpressure error.

The IPC envelope must never be sent to a Betaflight serial receiver. Only bytes
returned by Advance belong to CRSF. Their timestamps identify when each byte has
finished virtual UART serialization and is available to the FC parser.

## Optional downlink experiment

EnableDownlink sets telemetry denominator N, defaulting to 2 for empty payloads.
TransmitTelemetry/ReceiveTelemetry use slots where `(slot + 1) % N == 0`;
Transmit/Receive use the remaining slots. This preserves the original even RC /
odd telemetry timeline at 1:2. The downlink experiment starts at nonce 1:
`OtaNonce = (slot + 1) % 256`, so the reserved telemetry slots have nonce
divisible by N, matching the production HybridWide switch/ACK multiplexing.
RC-only mode retains `OtaNonce = slot % 256`. Both endpoints must use the same
revised harness; the outer IPC and CRSF/OTA packet formats are unchanged.
The configured UID, frequency-hop sequence, nonce and packet CRC apply in both
directions. Telemetry delivery cannot precede airtime completion and duplicate
delivered telemetry slots are rejected. Omit a delivery to model a lost packet.

QueueTelemetry uses the production RX queue, including its normal replacement
rules. Above 75 percent occupancy, host admission returns explicit backpressure
before feeding another frame. The caller may drop and count it or retry using a
new request sequence later; replaying an identical IPC request returns its cached
response. Legacy TelemetryIn is disabled in downlink mode because it drains the
queue instead of transmitting it.

RX StubbornSender fragmentation and TX StubbornReceiver reassembly use real OTA
telemetry data payloads. Confirmations travel in subsequent RC packets through
the production pack/unpack functions. A completed response carries the original
CRSF frame before any handset/backpack address adaptation. It is not a physical
handset UART implementation. The host reserves every Nth slot for telemetry. With EnableUplink it includes
link-statistics acknowledgements and the production burst limit. Synchronization
and the complete embedded slot scheduler remain outside this model.

## Reference uplink and lifecycle

V2 requires rate index 10, switch mode 0 (wide), 8-byte OTA and telemetry denominator
2 on both peers. Every other uplink opportunity remains RC. Reference snapshots
use production DF3 fragmentation/parity; management messages and session commands
use StubbornSender. Reference fragments also carry the downlink ACK bit. The RX
consumes session commands locally and forwards completed D5 frames as ordinary
CRSF UART bytes. A legacy peer does not negotiate v2 and receives no marked stream.

Empty EnableUplink selects an explicitly historical comparison fixture: latest
waiting D5 references share the reliable uplink with ordered management messages.
This path is native-test-only. Unknown versions and repeated enable requests fail
without altering the previously selected mode. Malformed QueueUplink requests do
not replace pending references or advance virtual time.

ReferenceStatus reports TX state 0=disabled, 1=negotiating, 2=streaming, 3=failed;
its second byte indicates readiness (matching RX heartbeat with a completed
reference). RX reports active-session and completed-reference booleans instead.
Status reads do not advance time. ReferenceReset advances time and clears the
session, assembly/sender, pending ingress and reference-health bookkeeping. It
preserves configuration, RF slot history, reliable management/telemetry queues,
UART events and the selected uplink mode. Use a fresh process for a full reboot.

The IPC version remains 1; these are additive operations. The DF3 radio transport
version is separately 2. See [the production transport contract](../src/lib/Df3/README.md).
