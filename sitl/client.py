"""Bounded, synchronous ELRS SITL IPC. No third-party packages."""
import socket
import struct
import subprocess
import tempfile
from pathlib import Path

MAGIC = 0x4553494C
HEADER = struct.Struct('!IHHIIQ')
HELLO, CONFIGURE, TRANSMIT, RECEIVE, ADVANCE, TELEMETRY, QUIT = range(1, 8)
ENABLE_DOWNLINK, QUEUE_TELEMETRY, TRANSMIT_TELEMETRY, RECEIVE_TELEMETRY = range(8, 12)


def packet(op, seq, time_us, payload=b''):
    if len(payload) > 4096:
        raise ValueError('payload exceeds 4096 bytes')
    return HEADER.pack(MAGIC, 1, op, len(payload), seq, time_us) + payload


def receive_exact(sock, length):
    out = bytearray()
    while len(out) < length:
        data = sock.recv(length - len(out))
        if not data:
            raise ConnectionError('ELRS SITL disconnected before completing the response')
        out.extend(data)
    return bytes(out)


class Peer:
    """One process/connection owns one firmware session; close tears both down."""
    def __init__(self, binary, expected_exit=0):
        self.directory = tempfile.TemporaryDirectory(prefix='elrs-')
        self.path = str(Path(self.directory.name) / 'ipc.sock')
        self.seq = 0
        self.time = 0
        self.expected_exit = expected_exit
        self.process = subprocess.Popen([str(binary), '--socket', self.path],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        try:
            import select
            ready, _, _ = select.select([self.process.stdout], [], [], 5)
            if not ready or not self.process.stdout.readline().startswith('READY '):
                raise RuntimeError('ELRS process did not become ready')
            self.sock.connect(self.path)
        except BaseException as error:
            self.close()
            if isinstance(error, RuntimeError):
                raise RuntimeError(f'{error}: {self.stderr.strip()}') from error
            raise

    def response(self, op, seq, time_us):
        magic, version, kind, length, sequence, time = HEADER.unpack(receive_exact(self.sock, 24))
        if (magic, version, kind, sequence, time) != (MAGIC, 1, op | 0x8000, seq, time_us) or not 4 <= length <= 4096:
            raise RuntimeError('invalid IPC response header')
        data = receive_exact(self.sock, length)
        status, = struct.unpack('!I', data[:4])
        if status:
            raise ValueError(data[4:].decode())
        return data[4:]

    def call(self, op, time_us=0, payload=b'', *, fragment=False):
        self.seq += 1
        self.last = packet(op, self.seq, time_us, payload)
        self.last_args = (op, self.seq, time_us)
        if fragment:
            for value in self.last:
                self.sock.sendall(bytes([value]))
        else:
            self.sock.sendall(self.last)
        result = self.response(op, self.seq, time_us)
        self.time = max(self.time, time_us)
        return result

    def retry(self):
        """Replay last request on the same healthy session; no reconnect retries."""
        self.sock.sendall(self.last)
        return self.response(*self.last_args)

    def configure(self, rate=6, mode=0, uid=b'\x01\x02\x03\x04\x05\x06', baud=420000):
        result = self.call(CONFIGURE, payload=uid + struct.pack('!BBI', rate, mode, baud))
        self.interval, self.airtime, self.packet_size, self.hop_interval = struct.unpack('!IIBB', result)
        return result

    def transmit(self, time_us, channels):
        return self.call(TRANSMIT, time_us, struct.pack('!16H', *channels))

    def advance(self, time_us):
        data = self.call(ADVANCE, time_us)
        n, = struct.unpack('!H', data[:2])
        if len(data) != 2 + 9 * n:
            raise RuntimeError('bad UART event length')
        return [struct.unpack_from('!QB', data, 2 + 9 * i) for i in range(n)]

    def close(self):
        if hasattr(self, 'sock'):
            self.sock.close()
        if hasattr(self, 'process'):
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=2)
            self.stderr = self.process.stderr.read()
            self.process.stdout.close()
            self.process.stderr.close()
        self.directory.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        if not args[0] and self.process.returncode != self.expected_exit:
            raise RuntimeError(f'ELRS process exited {self.process.returncode}: {self.stderr}')


def crc8(data):
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ (0xD5 if crc & 0x80 else 0)) & 255
    return crc


def decode_crsf(frame):
    """Independent wire check; Betaflight's C parser is a separate integration check."""
    if len(frame) != 26 or frame[:3] != b'\xc8\x18\x16' or crc8(frame[2:-1]) != frame[-1]:
        raise ValueError('invalid CRSF RC frame')
    packed = int.from_bytes(frame[3:-1], 'little')
    return [(packed >> (11*i)) & 2047 for i in range(16)]
