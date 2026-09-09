import argparse
import socket
import signal
import struct
import sys
import unittest
import subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from client import Peer, HELLO, CONFIGURE, TRANSMIT, RECEIVE, ADVANCE, TELEMETRY, QUIT, packet, decode_crsf, crc8

BINS = None

class SitlTests(unittest.TestCase):
    def peers(self, rate=6, mode=0):
        tx = self.enterContext(Peer(BINS / 'elrs_sitl_tx'))
        rx = self.enterContext(Peer(BINS / 'elrs_sitl_rx'))
        self.assertIn(b'role=tx', tx.call(HELLO))
        tx.configure(rate, mode)
        rx.configure(rate, mode)
        return tx, rx

    def roundtrip(self, tx, rx, t, channels):
        ota = tx.transmit(t, channels)
        self.assertEqual(rx.call(RECEIVE, t + tx.airtime, ota), b'\x01')
        events = rx.advance(t + tx.airtime + 620)
        self.assertEqual(len(events), 26)
        self.assertEqual(events[-1][0], t + tx.airtime + 620)
        return decode_crsf(bytes(b for _, b in events)), ota, events

    def test_real_codec_and_uart_boundaries(self):
        tx, rx = self.peers()
        channels = [992] * 16
        channels[2] = 911
        channels[4] = 1792
        ota = tx.transmit(0, channels)
        self.assertEqual(len(ota), 21)
        self.assertEqual(rx.call(RECEIVE, tx.airtime, ota), b'\x01')
        self.assertEqual(rx.advance(tx.airtime + 23), [])
        first = rx.advance(tx.airtime + 24)
        self.assertEqual(first, [(tx.airtime + 24, 0xc8)])
        rest = rx.advance(tx.airtime + 620)
        decoded = decode_crsf(bytes(b for _, b in first + rest))
        self.assertLessEqual(abs(decoded[2] - 911), 2)
        self.assertEqual(decoded[4], 1792)
        self.assertEqual(rx.advance(tx.airtime + 621), [])

    def test_crc_loss_frequency_and_nonce_wrap(self):
        tx, rx = self.peers()
        channels = [992] * 16
        channels[4] = 191
        hops = set()
        for slot in range(270):
            t = slot * tx.interval
            ota = bytearray(tx.transmit(t, channels))
            hops.add(bytes(ota[8:12]))
            if slot % 31 == 0:
                continue  # Dropped radio packet; no new CRSF output.
            corrupt = slot % 23 == 0
            if corrupt:
                ota[-1] ^= 1
            self.assertEqual(rx.call(RECEIVE, t + tx.airtime, ota), bytes([not corrupt]))
            events = rx.advance(t + tx.airtime + 620)
            self.assertEqual(len(events), 0 if corrupt else 26)
        self.assertGreater(len(hops), 20)
        ota = bytearray(tx.transmit(270 * tx.interval, channels));ota[8] ^= 1
        self.assertEqual(rx.call(RECEIVE, 270 * tx.interval + tx.airtime, ota), b'\x00')
        self.assertEqual(rx.advance(271 * tx.interval), [])

    def test_uid_mismatch_rejected(self):
        tx, rx = self.peers()
        # CRC initializer differs through UID[5]; independent process prevents shared state.
        other = self.enterContext(Peer(BINS / 'elrs_sitl_rx'))
        other.configure(uid=b'\x01\x02\x03\x04\x05\x07')
        ota = tx.transmit(0, [992]*16)
        self.assertEqual(other.call(RECEIVE, tx.airtime, ota), b'\x00')
        self.assertEqual(other.advance(10000), [])

    def test_modes_and_reproducible_processes(self):
        for rate, mode in [(6, 0), (6, 1), (5, 0), (5, 1), (8, 2), (10, 0)]:
            runs = []
            for _ in range(2):
                with Peer(BINS/'elrs_sitl_tx') as tx, Peer(BINS/'elrs_sitl_rx') as rx:
                    tx.configure(rate, mode);rx.configure(rate, mode)
                    output=[]
                    expected=[992]*16;expected[4]=191
                    for slot in range(20):
                        channels=[191+(slot*53+i*71)%1601 for i in range(16)]
                        channels[4]=1792 if slot%2 else 191
                        decoded, ota, events=self.roundtrip(tx,rx,slot*tx.interval,channels)
                        if tx.packet_size==13 and mode==1:
                            # Real 16-channel ELRS alternates banks; channels retain
                            # their previous value until their bank is transmitted.
                            bank=range(8,16) if slot%2 else range(8)
                            for i in bank:expected[i]=channels[i]
                            for a,b in zip(expected,decoded):self.assertLessEqual(abs(a-b),1)
                        else:
                            for a,b in zip(channels[:4],decoded[:4]):self.assertLessEqual(abs(a-b),2)
                            self.assertLessEqual(abs(decoded[4]-channels[4]), 1)
                        output.append((ota,events))
                    runs.append(output)
            self.assertEqual(runs[0],runs[1])

    def test_ipc_fragmentation_retry_and_transaction_errors(self):
        tx, rx=self.peers()
        payload=struct.pack('!16H', *([992]*16))
        ota=tx.call(TRANSMIT,0,payload,fragment=True)
        self.assertEqual(tx.retry(),ota)
        accepted=rx.call(RECEIVE,rx.airtime,ota)
        self.assertEqual(rx.retry(),accepted)
        events=rx.call(ADVANCE,rx.airtime+620)
        self.assertEqual(rx.retry(),events)  # Retrying drain returns same events, not a second drain.
        self.assertEqual(rx.advance(rx.airtime+621),[])
        with self.assertRaisesRegex(ValueError,'monotonic'):rx.advance(0)
        with self.assertRaisesRegex(ValueError,'grid'):tx.transmit(1,[992]*16)
        with self.assertRaisesRegex(ValueError,'channel'):tx.transmit(tx.interval,[2048]*16)
        with self.assertRaisesRegex(ValueError,'truncated'):tx.call(TRANSMIT,tx.interval,b'\x00')
        self.roundtrip(tx,rx,tx.interval,[992]*16)
        with self.assertRaisesRegex(ValueError,'monotonic|configure once'):tx.configure()

    def test_bad_header_sequence_and_disconnect(self):
        for kind in ['oversize','version','sequence','truncated','changed_retry']:
            with Peer(BINS/'elrs_sitl_tx', expected_exit=1) as tx:
                if kind=='oversize':
                    raw=struct.pack('!IHHIIQ',0x4553494c,1,1,4097,1,0)
                elif kind=='version':
                    raw=struct.pack('!IHHIIQ',0x4553494c,2,1,0,1,0)
                elif kind=='sequence':raw=packet(HELLO,2,0)
                elif kind=='changed_retry':
                    tx.call(HELLO);raw=packet(HELLO,1,1)
                else:raw=packet(HELLO,1,0)[:12]
                tx.sock.sendall(raw);tx.sock.shutdown(socket.SHUT_WR)
                try:self.assertEqual(tx.sock.recv(1),b'')
                except ConnectionResetError:pass
                self.assertEqual(tx.process.wait(timeout=2),1)
                self.assertFalse(Path(tx.path).exists())
        with Peer(BINS/'elrs_sitl_tx') as tx:
            tx.sock.close();self.assertEqual(tx.process.wait(timeout=2),0)
            self.assertFalse(Path(tx.path).exists())

    def test_shutdown_socket_ownership_and_deadline(self):
        with Peer(BINS/'elrs_sitl_tx') as tx:
            other=subprocess.run([str(BINS/'elrs_sitl_rx'),'--socket',tx.path],
                                 text=True,capture_output=True,timeout=2)
            self.assertEqual(other.returncode,1)
            self.assertTrue(Path(tx.path).exists())
            self.assertIn(b'role=tx',tx.call(HELLO))
            tx.call(QUIT);self.assertEqual(tx.process.wait(timeout=2),0)
            self.assertFalse(Path(tx.path).exists())
        with Peer(BINS/'elrs_sitl_tx') as tx:
            tx.process.send_signal(signal.SIGTERM)
            self.assertEqual(tx.process.wait(timeout=2),0)
            self.assertFalse(Path(tx.path).exists())
        with Peer(BINS/'elrs_sitl_tx', expected_exit=1) as tx:
            tx.sock.sendall(packet(HELLO,1,0)[:12])
            self.assertEqual(tx.process.wait(timeout=7),1)
            self.assertFalse(Path(tx.path).exists())

    def test_airtime_and_queue_backpressure(self):
        tx,rx=self.peers()
        ota=tx.transmit(0,[992]*16)
        with self.assertRaisesRegex(ValueError,'airtime'):rx.call(RECEIVE,0,ota)
        self.assertEqual(rx.call(RECEIVE,rx.airtime,ota),b'\x01')
        with self.assertRaisesRegex(ValueError,'duplicate'):rx.call(RECEIVE,rx.airtime,ota)
        for slot in range(1,154):
            ota=tx.transmit(slot*tx.interval,[992]*16)
            rx.call(RECEIVE,slot*tx.interval+rx.airtime,ota)
        ota=tx.transmit(154*tx.interval,[992]*16)
        with self.assertRaisesRegex(ValueError,'queue full'):rx.call(RECEIVE,154*tx.interval+rx.airtime,ota)
        self.assertEqual(len(rx.advance(154*tx.interval+rx.airtime)),400)
        self.assertEqual(rx.call(RECEIVE,154*tx.interval+rx.airtime,ota),b'\x01')

    def test_fc_telemetry_real_parser(self):
        _,rx=self.peers()
        # Standard CRSF battery payload: voltage, current, consumed capacity, remaining percent.
        frame=b'\xc8\x0a\x08'+struct.pack('!HH',42,17)+b'\x00\x00\x0c\x63'
        frame+=bytes([crc8(frame[2:])])
        parsed=rx.call(TELEMETRY,0,frame)
        self.assertEqual(parsed,frame)
        with self.assertRaisesRegex(ValueError,'invalid CRSF'):rx.call(TELEMETRY,0,frame[:-1]+b'\x00')

    def test_betaflight_production_parser(self):
        probe=BINS/'betaflight_crsf_probe'
        if not probe.exists():self.skipTest('BETAFLIGHT_SOURCE was not configured')
        tx,rx=self.peers()
        expected=[];stream=[]
        for slot in range(50):
            channels=[992]*16;channels[2]=191+slot*30;channels[4]=1792
            decoded,_,events=self.roundtrip(tx,rx,slot*tx.interval,channels)
            stream.extend(events);expected.append((events[-1][0],*decoded))
        source=''.join(f'{t} {b}\n' for t,b in stream)
        result=subprocess.run([str(probe)],input=source,text=True,capture_output=True,check=True,timeout=5)
        actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
        self.assertEqual(actual,expected)
        corrupted=list(stream[:26]);t,b=corrupted[-1];corrupted[-1]=(t,b^1)
        result=subprocess.run([str(probe)],input=''.join(f'{t} {b}\n' for t,b in corrupted),
                              text=True,capture_output=True,timeout=5)
        self.assertEqual(result.returncode,3);self.assertEqual(result.stdout,'')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--bin-dir',type=Path,required=True)
    args,rest=parser.parse_known_args();BINS=args.bin_dir.resolve()
    unittest.main(argv=[sys.argv[0]]+rest)
