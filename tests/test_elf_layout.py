import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from audit_title import elf_layout


class ElfLayoutTests(unittest.TestCase):
    def test_ppu_segment_and_file_bounds(self):
        header = b'\x7fELF\x02\x02\x01' + bytes(9)
        header += struct.pack('>HHIQQQIHHHHHH', 2, 21, 1, 0x10000, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack('>IIQQQQQQ', 1, 5, 0, 0x10000, 0, 120, 256, 16)
        layout = elf_layout(header+segment)
        self.assertEqual(layout['segments'][0]['vaddr'], 0x10000)
        self.assertEqual(layout['segments'][0]['memsz'], 256)
        with self.assertRaises(ValueError):
            elf_layout(header+struct.pack('>IIQQQQQQ', 1, 5, 0, 0x10000, 0, 121, 256, 16))

    def test_embedded_32bit_spu(self):
        header = b'\x7fELF\x01\x02\x01' + bytes(9)
        header += struct.pack('>HHIIIIIHHHHHH', 2, 23, 1, 0, 52, 0, 0, 52, 32, 1, 40, 0, 0)
        segment = struct.pack('>IIIIIIII', 1, 0, 0, 0, 84, 128, 5, 16)
        layout = elf_layout(bytes(16)+header+segment, 16)
        self.assertEqual(layout['machine'], 23)
        self.assertEqual(layout['segments'][0]['flags'], 5)
        self.assertEqual(layout['segments'][0]['filesz'], 84)


if __name__ == '__main__':
    unittest.main()
