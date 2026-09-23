"""Actual native codecs/queues and negotiated v3 lifecycle; no hardware."""
import argparse
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from client import Peer, ENABLE_DOWNLINK, ENABLE_UPLINK, QUEUE_UPLINK, RECEIVE, TRANSMIT_TELEMETRY, RECEIVE_TELEMETRY, REFERENCE_STATUS, REFERENCE_RESET, crc8

def reference(epoch,seq,t,inactive):
    body=struct.pack('!BBHHI9hhH',2,1 if inactive else 6,epoch,seq,t//1000,*([0]*10),200)
    f=bytes([0xee,36,0xd5,0xc8,0xea])+body
    return f+bytes([crc8(f[2:])])

def scenario(bin_dir,kind):
    received=[];status=[];markers=0;uart=bytearray();epoch=7;seq=0;rf_rc=[];local_rc=[]
    with Peer(bin_dir/'elrs_sitl_tx') as tx,Peer(bin_dir/'elrs_sitl_rx') as rx:
        for peer in (tx,rx):
            peer.configure(rate=10,mode=0);peer.call(ENABLE_DOWNLINK,payload=b'\x02')
            peer.call(ENABLE_UPLINK,payload=b'' if kind=='legacy_rx' and peer is rx else b'\x03')
        for t in range(0,8000000,2000):
            if kind=='rx_reset' and t==3000000:rx.call(REFERENCE_RESET,t)
            if kind=='epoch_reset' and t==3000000:epoch=8;seq=0
            armed=(kind=='rx_reset' and 1000000<=t<4000000 or
                   kind=='producer_stop_armed' and 1000000<=t<5000000 or
                   kind=='reference_gap' and 1000000<=t<6000000)
            channels=[992,992,350 if armed else 172,992,1792 if armed else 191]+[191]*11
            if t%50000==0:
                if not (kind.startswith('producer_stop') and t>=3000000 or
                        kind=='reference_gap' and 2000000<=t<4200000):
                    tx.call(QUEUE_UPLINK,t,reference(epoch,seq,t,not armed));seq+=1
                status.append((t,tx.call(REFERENCE_STATUS,t)[1]))
            if t%4000==0:
                packet=tx.transmit(t,channels);marker=(packet[13]&3)==1 and packet[14]&64
                markers+=bool(marker)
                if packet[13]&3==0:rf_rc.append(t)
                assert rx.call(RECEIVE,t+tx.airtime,packet)==b'\x01'
            else:
                packet=rx.call(TRANSMIT_TELEMETRY,t)
                if packet:assert tx.call(RECEIVE_TELEMETRY,t+tx.airtime,packet)[0]==1
            for stamp,b in rx.advance(t+1999):
                uart.append(b)
                while len(uart)>=2 and len(uart)>=uart[1]+2:
                    n=uart[1]+2;f=bytes(uart[:n]);del uart[:n];assert f[-1]==crc8(f[2:-1])
                    if f[2]==0xd5:received.append((stamp,int.from_bytes(f[7:9],'big'),int.from_bytes(f[9:11],'big')))
                    elif f[2]==0x16:local_rc.append((stamp,(int.from_bytes(f[3:-1],'little')>>(4*11))&2047))
        if kind=='legacy_rx':
            assert markers==0 and not received and not any(s for _,s in status)
        elif kind=='rx_reset':
            assert any(t<1000000 and s for t,s in status)
            assert not any(3200000<=t<4000000 for t,_,_ in received)
            assert any(3100000<=t<4100000 and not s for t,s in status)
            assert any(t>5000000 and s for t,s in status)
            assert any(t>5000000 for t,_,_ in received)
        elif kind.startswith('producer_stop'):
            assert any(1000000<t<2000000 and ready for t,ready in status)
            assert not any(1500000<t<3000000 for t in rf_rc)
            if kind=='producer_stop_armed':
                assert any(1500000<t<3000000 and arm>1700 for t,arm in local_rc)
                assert not any(1500000<t<5000000 for t in rf_rc), 'armed RC fallback'
                assert any(4900000<t<5000000 and arm>1700 for t,arm in local_rc), 'reference loss stopped FC channels'
                assert not any(t>3100000 for t,_,_ in received), 'stale reference forwarded'
                assert any(4100000<t<5000000 and not ready for t,ready in status), 'stale reference reported ready'
                assert any(5000000<t<5100000 and arm<500 for t,arm in local_rc)
            else:
                assert any(3300000<t<3400000 for t in rf_rc), 'disarmed RC did not resume'
        elif kind=='reference_gap':
            assert not any(2100000<t<4200000 for t,_,_ in received)
            assert any(4100000<t<4200000 and arm>1700 for t,arm in local_rc)
            assert any(4200000<t<4300000 for t,_,_ in received), 'fresh armed references did not recover'
            assert any(3100000<t<4000000 and not ready for t,ready in status)
            # The next 1 Hz heartbeat reports recovery while still armed.
            assert any(4200000<t<5500000 and ready for t,ready in status)
        else:
            assert any(e==7 for _,e,_ in received) and any(e==8 for _,e,_ in received)
            assert all(e==8 for t,e,_ in received if t>3500000)
            assert status[-1][1]
    print('PASS:',kind)

def rejected_requests_preserve_stream(bin_dir):
    channels = [992, 992, 172, 992, 172] + [172] * 11
    with Peer(bin_dir / 'elrs_sitl_tx') as tx:
        tx.configure(rate=10, mode=0)
        tx.call(ENABLE_DOWNLINK, payload=b'\x02')
        # Invalid version/body must not install a handler or change the mode.
        for body in (b'\x00', b'\x01', b'\x02', b'\x03\x00'):
            try:
                tx.call(ENABLE_UPLINK, payload=body)
            except ValueError:
                pass
            else:
                raise AssertionError('unsupported uplink request accepted')
        tx.call(ENABLE_UPLINK, payload=b'\x03')
        for body in (b'', b'\x03'):
            try:
                tx.call(ENABLE_UPLINK, payload=body)
            except ValueError:
                pass
            else:
                raise AssertionError('duplicate uplink enable accepted')
        tx.call(QUEUE_UPLINK, 0, reference(7, 0, 0, True))
        # CRC-valid but invalid D5 must not replace the pending valid reference,
        # nor advance virtual time when the request is rejected.
        bad = bytearray(reference(8, 1, 100000, True))
        bad[5] = 99
        bad[-1] = crc8(bad[2:-1])
        try:
            tx.call(QUEUE_UPLINK, 100000, bytes(bad))
        except ValueError:
            pass
        else:
            raise AssertionError('malformed reference accepted')
        tx.transmit(0, channels)
        state = tx.call(REFERENCE_STATUS)
        assert state[0] == 1 and int.from_bytes(state[6:8], 'big') == 7, state
    with Peer(bin_dir / 'elrs_sitl_tx') as tx:
        tx.configure(rate=10, mode=0)
        tx.call(ENABLE_DOWNLINK)
        tx.call(ENABLE_UPLINK, payload=b'\x03')
        tx.call(QUEUE_UPLINK, 0, reference(7, 0, 0, True))
        tx.call(REFERENCE_RESET)
        tx.transmit(0, channels)
        assert tx.call(REFERENCE_STATUS)[0] == 0, 'reset retained pending ingress'
        tx.call(QUEUE_UPLINK, 4000, reference(8, 0, 4000, True))
        tx.transmit(4000, channels)
        assert tx.call(REFERENCE_STATUS, 4000)[0] == 1
    with Peer(bin_dir / 'elrs_sitl_tx') as tx:
        tx.configure(rate=10, mode=0)
        tx.call(ENABLE_DOWNLINK)
        tx.call(ENABLE_UPLINK, payload=b'\x03')
        tx.call(QUEUE_UPLINK, 0, reference(7, 0, 0, True))
        tx.transmit(16000, channels)
        assert tx.call(REFERENCE_STATUS, 16000)[0] == 0, 'stale ingress started handshake'
    print('PASS: rejected requests, reset and stale ingress preserve admission rules')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--bin-dir',type=Path,required=True);args=parser.parse_args()
    rejected_requests_preserve_stream(args.bin_dir)
    for kind in ('legacy_rx','rx_reset','epoch_reset','producer_stop','producer_stop_armed','reference_gap'):scenario(args.bin_dir,kind)
    for mode,ratio in ((1,2),(0,4)):
        with Peer(args.bin_dir/'elrs_sitl_tx') as tx:
            tx.configure(rate=10,mode=mode);tx.call(ENABLE_DOWNLINK,payload=bytes([ratio]))
            try:tx.call(ENABLE_UPLINK,payload=b'\x03')
            except ValueError:pass
            else:raise AssertionError('unsupported profile enabled')
    print('PASS: unsupported profiles rejected')
