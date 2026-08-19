#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


BUILD_ENV = "Unified_ESP32S3_2400_TX_via_UART"
CHUNK_SIZE = 256 * 1024


def find_upload_port():
    ports = sorted(Path("/dev/serial/by-id").glob("usb-Espressif_USB_JTAG_serial_debug_unit_*-if00"))
    if len(ports) != 1:
        raise SystemExit("Specify the ESP32-S3 port with --upload-port")
    return str(ports[0])


def wait_for_port(port):
    for _ in range(100):
        if os.path.exists(port):
            return
        time.sleep(0.1)
    raise SystemExit(f"Upload port did not reappear: {port}")


def flash(esptool, port, images):
    wait_for_port(port)
    command = [
        sys.executable,
        str(esptool),
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        "460800",
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "--no-stub",
        "write_flash",
        "--no-compress",
        "--flash-mode",
        "keep",
        "--flash-freq",
        "keep",
        "--flash-size",
        "keep",
    ]
    for address, image in images:
        command.extend((hex(address), str(image)))
    subprocess.run(command, check=True)
    time.sleep(0.25)


def main():
    parser = argparse.ArgumentParser(description="Build and flash Nimbus over ESP32-S3 USB")
    parser.add_argument("--upload-port", help="ESP32-S3 USB serial port")
    args = parser.parse_args()

    project_dir = Path(__file__).resolve().parents[1]
    subprocess.run(("pio", "run", "-e", BUILD_ENV), cwd=project_dir, check=True)

    port = args.upload_port or find_upload_port()
    core_dir = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    esptool = core_dir / "packages" / "tool-esptoolpy" / "esptool.py"
    build_dir = project_dir / ".pio" / "build" / BUILD_ENV
    fixed_images = [
        (0x0000, build_dir / "bootloader.bin"),
        (0x8000, build_dir / "partitions.bin"),
        (0xE000, build_dir / "boot_app0.bin"),
    ]
    firmware = build_dir / "firmware.bin"

    required_files = [esptool, firmware, *(image for _, image in fixed_images)]
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        raise SystemExit(f"Missing flash input: {', '.join(missing)}")

    print("Flashing boot images")
    flash(esptool, port, fixed_images)

    # Short sessions avoid the USB Serial/JTAG reset seen during sustained writes.
    with tempfile.TemporaryDirectory(prefix="nimbus-flash-") as temporary_dir:
        with firmware.open("rb") as source:
            for index, data in enumerate(iter(lambda: source.read(CHUNK_SIZE), b"")):
                chunk = Path(temporary_dir) / f"firmware-{index:02d}.bin"
                chunk.write_bytes(data)
                address = 0x10000 + index * CHUNK_SIZE
                print(f"Flashing firmware chunk {index + 1} at {hex(address)}")
                flash(esptool, port, [(address, chunk)])

    print("Nimbus flash completed successfully")


if __name__ == "__main__":
    main()
