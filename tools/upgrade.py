"""UART image uploader for the STM32F411RE single-slot bootloader."""

import argparse
import struct
import sys
import zlib


APP_ADDR = 0x08008200
MAX_SIZE = 0x08080000 - APP_ADDR
CHUNK = 128


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def frame(command: int, seq: int, payload: bytes) -> bytes:
    if len(payload) > CHUNK or not 0 <= seq <= 65535:
        raise ValueError("invalid packet")
    body = b"BL" + struct.pack("<BHH", command, seq, len(payload)) + payload
    return body + struct.pack("<H", crc16(body))


def read_reply(port, command: int, seq: int) -> int:
    # Ignore the human-readable BOOT READY banner or stale bytes before a frame.
    while True:
        if port.read(1) != b"B":
            raise TimeoutError("no response from Bootloader")
        if port.read(1) != b"L":
            continue
        header = port.read(5)
        if len(header) != 5:
            raise TimeoutError("incomplete ACK")
        cmd, number, length = struct.unpack("<BHH", header)
        if length > CHUNK:
            raise ValueError("invalid ACK length")
        tail = port.read(length + 2)
        if len(tail) != length + 2:
            raise TimeoutError("incomplete ACK payload")
        reply = b"BL" + header + tail
        if crc16(reply) or cmd != command or number != seq or length != 1:
            raise ValueError("invalid ACK frame")
        return tail[0]


def send(port, command: int, seq: int, payload: bytes, retries: int = 3) -> None:
    for attempt in range(retries):
        port.write(frame(command, seq, payload))
        port.flush()
        try:
            status = read_reply(port, command, seq)
        except (TimeoutError, ValueError):
            if attempt + 1 == retries:
                raise
            continue
        if status:
            raise RuntimeError(f"device error status={status}, command={command}, seq={seq}")
        return


def check_image(image: bytes) -> None:
    if not 8 <= len(image) <= MAX_SIZE:
        raise ValueError(f"image size must be 8..{MAX_SIZE} bytes")
    sp, pc = struct.unpack_from("<II", image)
    if not (0x20000000 < sp <= 0x20020000 and sp % 8 == 0):
        raise ValueError("invalid initial stack pointer; check linker script")
    if not (pc & 1 and APP_ADDR <= (pc & ~1) < APP_ADDR + len(image)):
        raise ValueError("invalid reset vector; image must link at 0x08008200")


def upgrade(port, image: bytes) -> None:
    check_image(image)
    checksum = zlib.crc32(image)
    send(port, 1, 0, struct.pack("<II", len(image), checksum))
    total = (len(image) + CHUNK - 1) // CHUNK
    for seq in range(total):
        send(port, 2, seq, image[seq * CHUNK:(seq + 1) * CHUNK])
        print(f"\rWriting {seq + 1}/{total} packets", end="", flush=True)
    print()
    send(port, 3, total, b"")
    print(f"Upgrade verified, {len(image)} bytes, CRC32={checksum:08x}")


def main() -> None:
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyACM0")
    parser.add_argument("--image", required=True, help="application firmware.bin")
    args = parser.parse_args()
    with open(args.image, "rb") as file:
        image = file.read()
    check_image(image)
    with serial.Serial(args.port, 115200, timeout=20, write_timeout=5) as port:
        input("Press board RESET, then press Enter within 8 seconds to upload...")
        port.reset_input_buffer()
        upgrade(port, image)


if __name__ == "__main__":
    try:
        main()
    except (OSError, TimeoutError, ValueError, RuntimeError) as exc:
        print(f"Upgrade failed: {exc}", file=sys.stderr)
        sys.exit(1)
