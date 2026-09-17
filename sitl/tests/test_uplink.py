#!/usr/bin/env python3
"""Native OTA/Stubborn uplink, linkstats ACK, UART and queue freshness tests."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from client import (Peer, ENABLE_DOWNLINK, ENABLE_UPLINK, QUEUE_UPLINK,
                    TRANSMIT_TELEMETRY, RECEIVE_TELEMETRY, RECEIVE,
                    QUEUE_TELEMETRY, crc8)


def reference(sequence):
    # Extended 32-byte DF3 v1 payload. Distinct complete payload per source step.
    payload = struct.pack('!BBHHI9hhH', 1, 0, 7, sequence, sequence*10,
                          sequence, -sequence, -100, 123, -456, 789,
                          1000, -2000, 3000, 1234, 200)
    assert len(payload) == 32
    frame = bytes([0xEE, len(payload)+4, 0xD5, 0xC8, 0xEA]) + payload
    return frame + bytes([crc8(frame[2:])])


def simulate(bins, ratio=2, drop_data_every=0, drop_first_ack=False, management=False):
    frames, received, uart, pending = {}, [], [], bytearray()
    management_frames, management_received = [], []
    counts = Counter()
    with Peer(bins/'elrs_sitl_tx') as tx, Peer(bins/'elrs_sitl_rx') as rx:
        for peer in (tx, rx):
            peer.configure(rate=10, mode=0)
            peer.call(ENABLE_DOWNLINK, payload=bytes([ratio]))
            peer.call(ENABLE_UPLINK)
        channels = [992,992,191,992,191,191,1792] + [191]*9
        # A continuously queued FC telemetry stream competes with uplink ACKs.
        telemetry = bytes.fromhex('c8081e00000000000030')
        lost_ack = False
        for t in range(0, 2_000_000, tx.interval):
            slot = t//tx.interval
            if t % 10000 == 0:
                seq = t//10000
                frame = reference(seq); frames[seq] = frame
                tx.call(QUEUE_UPLINK, t, frame)
            if management and t == 20000:
                # Distinct FC parameter-read requests queued behind an active
                # reference. References continue at 100 Hz throughout the test.
                for parameter in range(1, 9):
                    body = bytes([0x2C, 0xC8, 0xEA, parameter, 0])
                    message = bytes([0xEE, len(body)+1]) + body + bytes([crc8(body)])
                    management_frames.append(message)
                    tx.call(QUEUE_UPLINK, t, message)
            if t % 40000 == 0:
                rx.call(QUEUE_TELEMETRY, t, telemetry)
            if (slot+1) % ratio:
                ota = tx.transmit(t, channels)
                kind = ota[13] & 3
                counts[f'ota_type_{kind}'] += 1
                drop = kind == 1 and drop_data_every and counts['ota_type_1'] % drop_data_every == 0
                if drop:
                    counts['uplink_dropped'] += 1
                else:
                    assert rx.call(RECEIVE, t+tx.airtime, ota) == b'\x01'
            else:
                ota = rx.call(TRANSMIT_TELEMETRY, t)
                if ota:
                    link = ota[14] & 3 == 1
                    counts['linkstats' if link else 'telemetry_data'] += 1
                    if link and t > 4000 and drop_first_ack and not lost_ack:
                        lost_ack = True
                        counts['ack_dropped'] += 1
                    else:
                        response = tx.call(RECEIVE_TELEMETRY, t+tx.airtime, ota)
                        assert response[0] == 1
                        if len(response) > 1:
                            assert response[1:] == telemetry
                            counts['telemetry_delivered'] += 1
            for stamp, value in rx.advance(t+tx.interval-1):
                uart.append((stamp,value)); pending.append(value)
                while len(pending) >= 2 and len(pending) >= pending[1]+2:
                    length = pending[1]+2
                    packet = bytes(pending[:length]); del pending[:length]
                    assert packet[0] == 0xC8 and crc8(packet[2:-1]) == packet[-1]
                    if packet[2] == 0xD5:
                        seq = int.from_bytes(packet[9:11], 'big')
                        assert packet == b'\xc8' + frames[seq][1:], 'Mixed or corrupt reference'
                        received.append({'sequence':seq, 'delivered_us':stamp,
                                         'source_us':seq*10000, 'age_us':stamp-seq*10000})
                    elif packet[2] == 0x2C and management:
                        management_received.append(packet)
                    else:
                        assert packet[2] == 0x16
                        counts['rc_uart_frames'] += 1
        assert received and len(received) >= (4 if ratio==8 else 8), received
        sequences = [entry['sequence'] for entry in received]
        assert all(b>a for a,b in zip(sequences,sequences[1:])), 'Replayed/out-of-order delivery'
        assert max(b-a for a,b in zip(sequences,sequences[1:])) > 1, 'Old queued references not superseded'
        assert counts['rc_uart_frames'] >= 200
        if management:
            assert management_received == [b'\xc8'+p[1:] for p in management_frames], 'Management starved, reordered or corrupted'
            assert max(b['delivered_us']-a['delivered_us'] for a,b in zip(received,received[1:])) < 150000, 'References starved by management'
        if ratio == 2:
            assert counts['telemetry_delivered'] > 0
        else:
            # Characterize a real bandwidth limitation, not a supported assist
            # configuration: saturated MSP ACKs consume every 1:8 downlink slot.
            assert counts['telemetry_delivered'] == 0
        if not drop_data_every and not drop_first_ack:
            assert max(entry['age_us'] for entry in received) < (300000 if ratio==8 else 85000)
        # Reject malformed frames before firmware queue admission.
        bad = bytearray(reference(201)); bad[-1] ^= 1
        try:
            tx.call(QUEUE_UPLINK, 2000000, bytes(bad))
            raise AssertionError('Bad CRSF CRC admitted')
        except ValueError as error:
            assert 'invalid' in str(error)
    return {'ratio':ratio, 'supported_assist_configuration':ratio==2,
            'telemetry_starved':counts['telemetry_delivered']==0,
            'drop_data_every':drop_data_every, 'drop_first_ack':drop_first_ack,
            'counts':dict(counts), 'references':received,
            'management_frames_delivered':len(management_received),
            'max_reference_age_us':max(entry['age_us'] for entry in received)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = {'scope':__doc__, 'passed':True,
              'cases':[simulate(args.bin_dir.resolve()),
                       simulate(args.bin_dir.resolve(), drop_data_every=13),
                       simulate(args.bin_dir.resolve(), drop_first_ack=True),
                       simulate(args.bin_dir.resolve(), ratio=8),
                       simulate(args.bin_dir.resolve(), management=True)]}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'passed':True, 'cases':[
        {k:v for k,v in case.items() if k!='references'} for case in report['cases']]}, indent=2))
