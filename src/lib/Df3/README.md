# DF3 reference transport

This is production code shared by the TX, RX and native protocol tests. It has no
Arduino or MCU dependency. The embedded adapters in `src/tx_main.cpp` and
`src/rx_main.cpp` own hardware gates, locking, ISR handoff and UART output.

| File | Responsibility |
|---|---|
| `Df3ReferenceStream.h` | Validate D5, losslessly pack snapshots, fragment/parity, freshness and reassembly |
| `Df3ReferenceSession.h` | Fixed-profile negotiation, health timeout and local readiness encoding |
| `../Handset/NimbusLinkSnapshot.h` | Periodic local USB link context for SDK reconnects; no extra RF packets |

There are no separate production switches for parity, reliability or scheduling.
Transport version 2 defines one profile: rate-table index 10 (500 Hz DF), wide
switch mode, 8-byte OTA packets, and 1:2 telemetry. Runtime checks require a
connected matching model, normal CRSF operation and no binding/AirPort/MAVLink
mode. Changing the profile requires a coordinated protocol change on both radios.

## Data path

1. The SDK sends a complete 38-byte extended CRSF D5 reference. The TX validates
   it and replaces its pending ingress mailbox; it never enters the reliable
   management queue.
2. A matching active session admits the newest reference into `Sender`. One
   pending and one active snapshot bound memory and latency. A transition to an
   inactive reference preempts an unfinished active reference.
3. The sender losslessly compacts the frame to 32 bytes: version/flags, epoch,
   sequence, source timestamp, lease, signed position/velocity/acceleration/yaw,
   and a session-bound CRC16. It preserves all numeric values and metadata.
4. Eight four-byte fragments plus one XOR parity fragment use every other uplink
   opportunity. The other opportunities remain available to RC. A five-byte OTA
   payload contains the generation tag's low byte and four fragment bytes.
5. The RX ISR queues fragments and their arrival timestamps. The main loop
   reassembles a complete snapshot (recovering one lost fragment), validates it,
   then forwards the original D5 to the FC with the FC UART address.

The OTA header uses bit 7 for the downlink acknowledgement, bit 6 as the stream
marker, bits 5:4 for the generation tag's upper bits, and bits 3:0 for fragment
index 0..8. Fragments have no reference retransmission/acknowledgement protocol.
The 100 ms send period, 12 ms start deadline and 80 ms finish/assembly deadlines
discard stale work. Duplicates cannot republish or renew a reference. The FC
still validates the full sequence, timestamp and lease after transport decoding.

## Session and readiness

The vendor command is CRSF `0x32`, vendor `0x80`, command `0x44`. HELLO, READY,
COMMIT and ACTIVE negotiate a nonzero session nonce, SDK epoch and the exact
profile bytes. Negotiation requires recent disarmed RC input. The normal
StubbornSender management channel carries these commands. Retries are 250 ms
apart and negotiation times out after 2 seconds.

The RX emits D9 health at 1 Hz. Its reference watchdog clears the session after
1 second without a completed reference. TX health expires after 2.5 seconds.
Readiness requires a matching heartbeat reporting a completed reference, not
just completion of negotiation. An SDK epoch change requires fresh negotiation.
The TX emits E5 readiness locally over USB at 250 ms intervals or immediately
when state/readiness changes. This status is not an RF reference packet.

Waiting D6 state and D9 health telemetry are replaced in place and prioritized;
an already active reliable downlink transfer remains immutable. Once the FC
publishes DF3 v1 state, waiting D1/D3 sensor diagnostics also become latest-only.

## Verification and historical experiments

The standalone tests under `sitl/tests` cover lifecycle, parity/loss, corruption,
ordering, duplicates, expiry, timer wrap, telemetry priority and SDK link context.
See [the native build instructions](../../../sitl/README.md).

`sitl/tests/fixtures/LegacyReferenceQueue.h` preserves the old reliable-D5
comparison used by empty-body EnableUplink. It is explicitly a test fixture and
is not compiled into either radio. Historical reports in the parent simulator
repository describe experiments, not additional production configuration flags.
