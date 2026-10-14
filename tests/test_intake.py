import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from inspect_input import parse_elf, parse_sfo


class IntakeTests(unittest.TestCase):
    def test_ppu_big_endian(self):
        data = b'\x7fELF\x02\x02\x01' + bytes(9)
        data += struct.pack('>HHIQQQIHHHHHH', 2, 21, 1, 0x10000, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        result = parse_elf(data, 120)
        self.assertEqual((result['machine_name'], result['entry'], result['endian']), ('PowerPC64/PPU', '0x10000', 'big'))
        self.assertTrue(result['program_headers_in_file'])
        self.assertFalse(parse_elf(data, 64)['program_headers_in_file'])

    def test_sfo_text_and_bad_offset(self):
        key, value = b'TITLE_ID\x00', b'NPUB31251\x00'
        header = struct.pack('<5I', 0x46535000, 0x101, 36, 36 + len(key), 1)
        record = struct.pack('<HHIII', 0, 0x0204, len(value), len(value), 0)
        self.assertEqual(parse_sfo(header + record + key + value)['values']['TITLE_ID'], 'NPUB31251')
        with self.assertRaises(ValueError):
            parse_sfo(header + record + key)

    def test_truncated(self):
        for data in (b'', b'\x00PSF' + bytes(8)):
            with self.assertRaises(ValueError):
                parse_sfo(data)
        with self.assertRaises(ValueError):
            parse_elf(b'\x7fELF\x02\x02' + bytes(10), 16)


if __name__ == '__main__':
    unittest.main()
