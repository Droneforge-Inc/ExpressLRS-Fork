# DF3 reference transport

This is production code shared by the TX, RX and native protocol tests. It has no
Arduino or MCU dependency. The embedded adapters in `src/tx_main.cpp` and
`src/rx_main.cpp` own hardware gates, locking, ISR handoff and UART output.

| File | Responsibility |
|---|---|
| `Df3ReferenceStream.h` | Validate D5, losslessly pack snapshots, fragment/parity, freshness and reassembly |
| `Df3ReferenceSession.h` | Negotiation, health/readiness and reference-to-channel permissions |
| `../Handset/NimbusLinkSnapshot.h` | Periodic local USB link context for SDK reconnects; no extra RF packets |

There are no separate production switches for parity, reliability or scheduling.
Transport version 3 defines one profile: rate-table index 10 (500 Hz DF), wide
switch mode, 8-byte OTA packets, and 1:2 telemetry. Runtime checks require a
connected matching model, normal CRSF operation and no binding/AirPort/MAVLink
mode. Changing the profile requires a coordinated protocol change on both radios.

## Data path

1. The SDK sends a complete 38-byte extended CRSF D5 v2 reference. The TX validates
   it and replaces its pending ingress mailbox; it never enters the reliable
   management queue.
2. A matching active session admits the newest reference into `Sender`. One
   pending and one active snapshot bound memory and latency. A transition to an
   inactive reference or explicit disarm preempts an unfinished active reference.
3. The sender losslessly compacts the frame to 32 bytes: flags, epoch,
   sequence, source timestamp, lease, signed position/velocity/acceleration/yaw,
   and a session-bound CRC16. It preserves all numeric values and metadata.
   The transport version is negotiated and bound to the session instead of
   repeated in each snapshot.
4. Eight four-byte fragments plus one XOR parity fragment can use every uplink
   opportunity. Native RC does not use RF slots while DF3 owns control. A five-byte OTA
   payload contains the generation tag's low byte and four fragment bytes.
5. The RX ISR queues fragments and their arrival timestamps. The main loop
   reassembles a complete snapshot (recovering one lost fragment), validates it,
   then converts it to the existing FC D5 v1 format. Numeric fields are unchanged.
   It also produces normal CRSF channel frames locally for Betaflight's arming,
   calibration and failsafe code. This requires no FC protocol change.

The OTA header uses bit 7 for the downlink acknowledgement, bit 6 as the stream
marker, bits 5:4 for the generation tag's upper bits, and bits 3:0 for fragment
index 0..8. Index 15 is an ACK-only idle packet: no data or lease renewal.
Fragments have no reference retransmission/acknowledgement protocol.
The 50 ms send period, 12 ms start deadline and 80 ms finish/assembly deadlines
discard stale work. Duplicates cannot republish or renew a reference. The FC
still validates the full sequence, timestamp and lease after transport decoding.

## Control permissions and loss

D5 v2 flags are inactive (bit 0), armed (1), Flight Assist (2), gyro calibration
(3), accelerometer calibration (4) and inflight calibration (5). Bits 6..7 are
reserved. Active references require armed + Flight Assist. Ground calibrations
are mutually exclusive and disarmed. Flags share the snapshot's sequence,
timestamp and CRC; partial references cannot change arm state.

The receiver emits local UART channels at 50 Hz while the radio/profile and DF3
session remain valid. Roll/pitch/yaw are neutral. AUX1 carries
arm, AUX2 selects Nimbus ANGLE, AUX3 carries assist and AUX5 carries inflight
calibration. Throttle is low before a trajectory and the existing idle permit
while it is active. Ground calibration uses the SDK's existing stick gestures.

References still expire after their source/arrival lease (200 ms from the SDK).
Expiry retains the last arm/assist state and ends calibration gestures; it does
not stop local RC output or reset the session. This lets DF3 use its reference-loss
hold/descent fallback without creating RX LOSS. Fresh armed references can resume
within the same session. Actual radio/profile/session loss clears permissions and
stops channels; a new session requires a fresh disarm before arming. Repeated UART
output and idle packets never renew reference freshness. Idle packets still
acknowledge telemetry after a session reset so failure health can reach TX.

The TX snapshots input timestamps before reading its comparison clock; otherwise
an arrival on the handset core can appear stale through unsigned underflow.
The RX drains queued fragments at their recorded arrival times before checking
current-time expiry. A completed reference must still be fresh at service time,
before it changes permissions or is forwarded; stale queued arm/calibration
commands cannot take effect.

SDK RC stays on USB as Nimbus's local handset/management input. Native RF RC is
used before negotiation and after explicitly leaving DF3 while disarmed. A
failed DF3 session cannot fall back to cached armed RC. At RX, only native disarm
can release reference ownership. Stopping the producer while USB RC is disarmed
returns TX to native RC after 250 ms. Ordinary RC operation is otherwise unchanged.

The SDK and both radios must agree on v3. Old SDK v1 references and old radio
transport v2 sessions cannot enable this mode; there is no silent mixed-version
fallback. Rebuild/update the matched components together before hardware testing.

## Session and readiness

The vendor command is CRSF `0x32`, vendor `0x80`, command `0x44`. HELLO, READY,
COMMIT and ACTIVE negotiate a nonzero session nonce, SDK epoch and the exact
profile bytes. Negotiation requires recent disarmed RC input. The normal
StubbornSender management channel carries these commands. Retries are 250 ms
apart and negotiation times out after 2 seconds.

The RX emits D9 health at 1 Hz. Reference expiry clears the heartbeat's readiness
bit while preserving the active session; a fresh reference restores it on the
next heartbeat. TX health expires after 2.5 seconds without a matching heartbeat.
Readiness requires a matching heartbeat reporting a fresh completed reference,
not just completion of negotiation. An SDK epoch change requires fresh negotiation.
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
