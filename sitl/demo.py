"""Run independent TX/RX processes, record OTA/UART traces, verify BF parser."""
import argparse
import csv
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from client import Peer, HELLO, RECEIVE, decode_crsf


def run(bin_dir, output, duration_us=3_000_000):
    output.mkdir(parents=True, exist_ok=True)
    uart=[];radio=[];decoded=[]
    with Peer(bin_dir/'elrs_sitl_tx') as tx, Peer(bin_dir/'elrs_sitl_rx') as rx:
        tx_info=tx.call(HELLO).decode();rx_info=rx.call(HELLO).decode()
        tx.configure();rx.configure()
        for t in range(0,duration_us,tx.interval):
            channels=[992]*16
            channels[2]=911 if 200_000<=t<2_600_000 else 191
            channels[0]=1023 if 1_000_000<=t<1_100_000 else 992
            channels[4]=1792 if 50_000<=t<2_800_000 else 191
            ota=tx.transmit(t,channels)
            accepted=rx.call(RECEIVE,t+tx.airtime,ota)
            if accepted!=b'\x01':raise RuntimeError('clean-link packet rejected')
            events=rx.advance(t+tx.airtime+620)
            frame=bytes(value for _,value in events)
            result=decode_crsf(frame)
            if abs(result[2]-channels[2])>2:raise RuntimeError('throttle roundtrip mismatch')
            uart.extend(events);decoded.append((events[-1][0],*result))
            slot,frequency,length=struct.unpack('!QIB',ota[:13])
            radio.append((t,t+tx.airtime,slot,frequency,ota[13:].hex(),frame.hex()))
        interval=tx.interval;airtime=tx.airtime
    for name,head,rows in [
        ('uart.csv',['time_us','byte'],uart),
        ('channels.csv',['frame_end_us']+[f'ch{i+1}_crsf' for i in range(16)],decoded),
        ('radio.csv',['tx_start_us','rx_complete_us','slot','sx1280_frequency_register','ota_hex','crsf_hex'],radio)]:
        with (output/name).open('w',newline='') as f:
            writer=csv.writer(f);writer.writerow(head);writer.writerows(rows)
    (output/'receiver.crsf').write_bytes(bytes(b for _,b in uart))
    probe=bin_dir/'betaflight_crsf_probe'
    verified=False
    if probe.exists():
        incoming=''.join(f'{t} {byte}\n' for t,byte in uart)
        run=subprocess.run([str(probe)],input=incoming,text=True,capture_output=True,check=True,timeout=5)
        actual=[tuple(map(int,line.split())) for line in run.stdout.splitlines()]
        if actual!=decoded:raise RuntimeError('Betaflight parser differs from receiver output')
        (output/'betaflight-decoded.txt').write_text(run.stdout)
        verified=True
    manifest=dict(tx=tx_info,rx=rx_info,duration_us=duration_us,frames=len(radio),uart_bytes=len(uart),
        radio_interval_us=interval,mode='HybridWide, 250 Hz, pre-synchronized RC-only slots',
        airtime_us=airtime,uart_baud=420000,uart_8n1_frame_us=620,
        betaflight_parser_verified=verified,
        uart_sha256=hashlib.sha256((output/'receiver.crsf').read_bytes()).hexdigest(),
        limitations=['No embedded TX/RX main loop, binding, sync acquisition, MCU scheduling or RF PHY',
          'No telemetry reservation slots, RF downlink, receiver timeout state machine or calibrated RF losses',
          'UART input to TX is raw channels; handset CRSF parser is not exercised',
          'Betaflight parser probe only; no live Betaflight SITL or MATLAB connection'])
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))
    return manifest

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--bin-dir',type=Path,default=Path('build/sitl'))
    p.add_argument('--output',type=Path,default=Path('build/sitl/demo'))
    a=p.parse_args();run(a.bin_dir.resolve(),a.output.resolve())
