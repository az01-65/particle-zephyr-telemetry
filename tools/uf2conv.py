#!/usr/bin/env python3
"""
uf2conv.py -- convert an Intel HEX file into a UF2 file, for Adafruit's
nRF52 UF2 bootloader's drag-and-drop flashing (no debug probe, no
OpenOCD, no Arduino IDE required once the bootloader itself is already
on the board - see tools/flash_uf2.sh for the wrapper that actually
copies the result onto a mounted board).

Written from scratch against the public UF2 format spec
(https://github.com/microsoft/uf2) - not copied from any existing
converter. Stdlib only, no dependencies.

Usage:
    uf2conv.py input.hex output.uf2 [--allow-low-address]

Refuses by default to convert a hex linked below the SoftDevice/
bootloader boundary (0x26000) - see to_uf2()'s BOOTLOADER_SAFE_MIN_ADDR
check - since writing one via UF2 would corrupt that territory.
--allow-low-address bypasses this for the rare legitimate case.
"""
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000

# Family ID for the nRF52840, from the UF2 project's registered family ID
# list (uf2families.json in the microsoft/uf2 repo) - this is what tells
# the bootloader "this file is for my chip, don't flash it blindly."
NRF52840_FAMILY_ID = 0xADA52840

BLOCK_SIZE = 512
DATA_SIZE = 476
PAYLOAD_SIZE = 256

# The Adafruit nRF52 UF2 bootloader occupies flash below this address
# (SoftDevice + bootloader itself). A hex linked below here was NOT built
# with the UF2/bootloader-safe overlay+conf (see firmware/blank_app's
# *_uf2.overlay / *_uf2.conf) and writing it via UF2 would overwrite that
# territory - see to_uf2()'s low-address guard below.
BOOTLOADER_SAFE_MIN_ADDR = 0x26000


def parse_ihex(path):
    """Parse an Intel HEX file into a sorted list of (address, bytearray)
    runs, with consecutive records merged into contiguous byte ranges."""
    chunks = []
    base = 0
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line[0] != ":":
                raise ValueError(f"{path}:{lineno}: not an Intel HEX record")
            raw = bytes.fromhex(line[1:])
            count = raw[0]
            addr = (raw[1] << 8) | raw[2]
            rtype = raw[3]
            data = raw[4:4 + count]
            checksum = raw[4 + count]
            calc = (-(sum(raw[:4 + count]))) & 0xFF
            if calc != checksum:
                raise ValueError(f"{path}:{lineno}: bad checksum")

            if rtype == 0x00:  # data
                chunks.append((base + addr, bytearray(data)))
            elif rtype == 0x01:  # end of file
                break
            elif rtype == 0x02:  # extended segment address
                base = ((data[0] << 8) | data[1]) << 4
            elif rtype == 0x04:  # extended linear address
                base = ((data[0] << 8) | data[1]) << 16
            elif rtype in (0x03, 0x05):  # start segment/linear address
                continue
            else:
                raise ValueError(f"{path}:{lineno}: unsupported record type {rtype:#x}")

    if not chunks:
        raise ValueError(f"{path}: no data records found")

    chunks.sort(key=lambda c: c[0])
    merged = [chunks[0]]
    for addr, data in chunks[1:]:
        last_addr, last_data = merged[-1]
        if addr == last_addr + len(last_data):
            last_data.extend(data)
        else:
            merged.append((addr, bytearray(data)))
    return merged


def to_uf2(chunks, family_id=NRF52840_FAMILY_ID, allow_low_address=False):
    """Split (address, bytes) runs into fixed 256-byte-payload UF2 blocks.
    The final partial block of each run is padded with 0xFF (the erased-
    flash value), never 0x00 - so a short final block never looks like it
    is deliberately zeroing flash it shouldn't touch."""
    blocks = []
    for addr, data in chunks:
        offset = 0
        while offset < len(data):
            payload = bytes(data[offset:offset + PAYLOAD_SIZE])
            if len(payload) < PAYLOAD_SIZE:
                payload += b"\xFF" * (PAYLOAD_SIZE - len(payload))
            blocks.append((addr + offset, payload))
            offset += PAYLOAD_SIZE

    min_addr = min(block_addr for block_addr, _ in blocks)
    if min_addr < BOOTLOADER_SAFE_MIN_ADDR and not allow_low_address:
        raise ValueError(
            f"refusing to convert: lowest address in this hex is "
            f"{min_addr:#x}, below {BOOTLOADER_SAFE_MIN_ADDR:#x}. This hex "
            f"was not linked with the UF2/bootloader-safe overlay+conf "
            f"(see firmware/blank_app's *_uf2.overlay / *_uf2.conf) and "
            f"writing it via UF2 would overwrite the SoftDevice/bootloader "
            f"itself. If you really mean to write this low an address "
            f"(rare - e.g. a deliberate direct SWD-equivalent use case), "
            f"pass --allow-low-address to override."
        )

    total = len(blocks)
    out = bytearray()
    for i, (block_addr, payload) in enumerate(blocks):
        header = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY_ID_PRESENT,
            block_addr,
            PAYLOAD_SIZE,
            i,
            total,
            family_id,
        )
        body = payload + b"\x00" * (DATA_SIZE - len(payload))
        footer = struct.pack("<I", UF2_MAGIC_END)
        out += header + body + footer
    return bytes(out)


def main():
    args = sys.argv[1:]
    allow_low_address = "--allow-low-address" in args
    positional = [a for a in args if a != "--allow-low-address"]

    if len(positional) != 2:
        print("usage: uf2conv.py input.hex output.uf2 [--allow-low-address]",
              file=sys.stderr)
        sys.exit(1)

    in_path, out_path = positional

    try:
        chunks = parse_ihex(in_path)
        uf2_bytes = to_uf2(chunks, allow_low_address=allow_low_address)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    with open(out_path, "wb") as f:
        f.write(uf2_bytes)

    total_bytes = sum(len(d) for _, d in chunks)
    print(f"Wrote {out_path}: {len(uf2_bytes) // BLOCK_SIZE} blocks, "
          f"{total_bytes} bytes of firmware")


if __name__ == "__main__":
    main()
