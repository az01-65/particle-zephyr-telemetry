#!/usr/bin/env python3
"""Unit test for uf2conv.py - run with: python3 tools/test_uf2conv.py"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uf2conv  # noqa: E402


class TestParseIhex(unittest.TestCase):
    def test_single_short_record(self):
        # 4 data bytes (DE AD BE EF) at address 0x0000, base address 0
        # (no extended linear/segment record needed - base defaults to 0).
        hexdata = ":04000000DEADBEEFC4\n:00000001FF\n"
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            chunks = uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)
        self.assertEqual(len(chunks), 1)
        addr, data = chunks[0]
        self.assertEqual(addr, 0)
        self.assertEqual(bytes(data), b"\xDE\xAD\xBE\xEF")

    def test_extended_linear_address(self):
        # Extended linear address record sets base = 0x00260000, then one
        # data record at offset 0x0000 -> absolute address 0x00260000.
        hexdata = (
            ":020000040026D4\n"
            ":04000000DEADBEEFC4\n"
            ":00000001FF\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            chunks = uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)
        self.assertEqual(len(chunks), 1)
        addr, data = chunks[0]
        self.assertEqual(addr, 0x00260000)
        self.assertEqual(bytes(data), b"\xDE\xAD\xBE\xEF")

    def test_extended_segment_address(self):
        # Extended segment address record (type 0x02, not 0x04) - this is
        # what the real shipped blank_app UF2 build actually uses. Value
        # 0x2600 is shifted left 4 bits per the format -> base 0x00026000,
        # then one data record at offset 0x0000 -> absolute address
        # 0x00026000. Checksum computed the same way as the other tests
        # here: two's-complement of the sum of all preceding bytes.
        #   count=0x02 addr=0x0000 type=0x02 data=0x26,0x00
        #   sum = 0x02+0x00+0x00+0x02+0x26+0x00 = 0x2A -> checksum = 0xD6
        hexdata = (
            ":020000022600D6\n"
            ":04000000DEADBEEFC4\n"
            ":00000001FF\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            chunks = uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)
        self.assertEqual(len(chunks), 1)
        addr, data = chunks[0]
        self.assertEqual(addr, 0x00026000)
        self.assertEqual(bytes(data), b"\xDE\xAD\xBE\xEF")

    def test_bad_checksum_rejected(self):
        hexdata = ":04000000DEADBEEF00\n:00000001FF\n"  # wrong checksum
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            with self.assertRaises(ValueError):
                uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)


class TestToUf2(unittest.TestCase):
    def test_single_small_block(self):
        chunks = [(0, b"\xDE\xAD\xBE\xEF")]
        out = uf2conv.to_uf2(chunks, allow_low_address=True)
        self.assertEqual(len(out), uf2conv.BLOCK_SIZE)  # exactly one block

        magic0, magic1, flags, block_addr, payload_size, block_no, num_blocks, family_id = (
            struct.unpack("<IIIIIIII", out[0:32])
        )
        self.assertEqual(magic0, uf2conv.UF2_MAGIC_START0)
        self.assertEqual(magic1, uf2conv.UF2_MAGIC_START1)
        self.assertEqual(flags, uf2conv.UF2_FLAG_FAMILY_ID_PRESENT)
        self.assertEqual(block_addr, 0)
        self.assertEqual(payload_size, uf2conv.PAYLOAD_SIZE)
        self.assertEqual(block_no, 0)
        self.assertEqual(num_blocks, 1)
        self.assertEqual(family_id, uf2conv.NRF52840_FAMILY_ID)

        payload = out[32:32 + uf2conv.PAYLOAD_SIZE]
        self.assertEqual(payload[:4], b"\xDE\xAD\xBE\xEF")
        self.assertEqual(payload[4:], b"\xFF" * (uf2conv.PAYLOAD_SIZE - 4))

        (magic_end,) = struct.unpack("<I", out[-4:])
        self.assertEqual(magic_end, uf2conv.UF2_MAGIC_END)

    def test_two_blocks_for_300_bytes(self):
        data = bytes(range(256)) + bytes(range(44))  # 300 bytes total
        out = uf2conv.to_uf2([(0x1000, data)], allow_low_address=True)
        self.assertEqual(len(out), 2 * uf2conv.BLOCK_SIZE)
        # second block's target address must follow the first payload
        block_addr2 = struct.unpack("<I", out[uf2conv.BLOCK_SIZE + 12:uf2conv.BLOCK_SIZE + 16])[0]
        self.assertEqual(block_addr2, 0x1000 + uf2conv.PAYLOAD_SIZE)


class TestLowAddressGuard(unittest.TestCase):
    def test_low_address_refused_by_default(self):
        # 0x1000 is well below the 0x26000 SoftDevice/bootloader boundary -
        # must be refused unless allow_low_address is explicitly passed.
        with self.assertRaises(ValueError):
            uf2conv.to_uf2([(0x1000, b"\xDE\xAD\xBE\xEF")])

    def test_low_address_allowed_with_override(self):
        out = uf2conv.to_uf2([(0x1000, b"\xDE\xAD\xBE\xEF")], allow_low_address=True)
        self.assertEqual(len(out), uf2conv.BLOCK_SIZE)

    def test_bootloader_safe_address_needs_no_override(self):
        # At/above the boundary, no flag should be required.
        out = uf2conv.to_uf2([(uf2conv.BOOTLOADER_SAFE_MIN_ADDR, b"\xDE\xAD\xBE\xEF")])
        self.assertEqual(len(out), uf2conv.BLOCK_SIZE)


if __name__ == "__main__":
    unittest.main()
