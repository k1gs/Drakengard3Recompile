import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from inspect_ue3_header import parse_prefix


class UE3PrefixTests(unittest.TestCase):
    def test_platform_endianness(self):
        for endian in ('>', '<'):
            data = struct.pack(endian+'IIIi', 0x9e2a83c1, 860, 64, 5)
            data += b'None\0' + struct.pack(endian+'7I', 0, 10, 64, 4, 200, 2, 180)
            result = parse_prefix(data, 1024)
            self.assertEqual((result['file_version'], result['licensee_version'], result['exports']), (860, 0, 4))
            self.assertEqual(result['endian'], 'big' if endian == '>' else 'little')

    def test_out_of_file_tables_and_bad_tag(self):
        data = struct.pack('>IIIi', 0x9e2a83c1, 860, 64, 5)
        data += b'None\0' + struct.pack('>7I', 0, 10, 64, 4, 200, 2, 180)
        with self.assertRaises(ValueError):
            parse_prefix(data, 100)
        with self.assertRaises(ValueError):
            parse_prefix(bytes(64), 1024)


if __name__ == '__main__':
    unittest.main()
