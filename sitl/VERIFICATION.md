# Verified native protocol SITL proof

Verified locally on 2026-09-05, Linux x86-64, GCC 16.1.1.

- ExpressLRS fork base: `55396672f208`; host changes are in the local working tree.
- Betaflight fork base: `aa0f5edcbe93`; parser compiled read-only from the existing checkout.
- Normal Debug build: 10 tests passed.
- AddressSanitizer + UndefinedBehaviorSanitizer build (nonrecovering): 10 tests passed.
- Three-second demo: 750 accepted OTA packets, 750 valid CRSF frames, 19,500 UART bytes.
- Betaflight's actual CRSF receive callback and frame decoder accepted every frame;
  all 16 decoded channels and frame completion timestamps matched the independent
  wire decoder. Corrupt CRSF CRCs were rejected by Betaflight's parser.
- Repeated fresh TX/RX processes produced byte-identical outputs and timestamps
  across HybridWide, Hybrid, 8/12/16-channel modes and the fork's custom DF rate.
- Covered OTA corruption, wrong UID/frequency, dropped frames, nonce wrap,
  premature reception, duplicate slots, queue backpressure, malformed IPC headers,
  fragmented input, identical retries, conflicting retries, time reversal, clean
  disconnect/QUIT/SIGTERM, socket ownership, and incomplete-message timeout.
- The real ELRS FC telemetry parser accepted a valid battery frame and invalid CRC
  input was rejected by the adapter.

Demo CRSF stream SHA-256:
`74e74f86f007f97cf04441a799e98762a30b27d4de7ae30e941a0eba6b3941b1`

Artifacts are regenerated under `build/sitl/demo/`; CTest logs are under
`build/sitl/Testing/Temporary/` and `build/sitl-asan/Testing/Temporary/`.

These results establish the bounded protocol proof described in README.md.
They do not establish full embedded ELRS application, real-radio latency,
RF downlink, binding/sync, failsafe, or live Betaflight/MATLAB integration fidelity.
