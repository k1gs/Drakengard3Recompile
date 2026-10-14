import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from inspect_coalesced import inventory, Reader


def n(value):
    return struct.pack('>i', value)


def s(value):
    return n(len(value)+1)+value.encode('ascii')+b'\0'


class CoalescedTests(unittest.TestCase):
    def test_nested_maps_and_exact_end(self):
        data=n(1)+s('test.ini')+n(1)+s('section')+n(1)+s('key')+s('value')
        result=inventory(data)
        self.assertEqual((result['sections'],result['pairs'],result['consumed']),(1,1,len(data)))
        with self.assertRaisesRegex(ValueError,'Trailing'):
            inventory(data+b'extra')

    def test_wide_and_empty_strings(self):
        r=Reader(n(-3)+'ok\0'.encode('utf-16-le')+n(0))
        self.assertEqual(r.string(),'ok')
        self.assertEqual(r.string(),'')
        self.assertEqual(Reader(n(-3)+'ok\0'.encode('utf-16-be'),'big').string(),'ok')

    def test_malformed_count(self):
        with self.assertRaisesRegex(ValueError,'Invalid count'):
            inventory(n(-1))

    def test_padding_requires_explicit_mode_and_zero_alignment(self):
        data=n(1)+s('x')+n(0)
        padded=data+b'\0'*((-len(data))%16)
        with self.assertRaisesRegex(ValueError,'Trailing'):
            inventory(padded)
        self.assertEqual(inventory(padded,True)['consumed'],len(data))
        with self.assertRaisesRegex(ValueError,'Trailing'):
            inventory(data+b'\1\0',True)

    def test_truncated_or_unterminated_string(self):
        with self.assertRaisesRegex(ValueError,'Truncated'):
            Reader(n(100)+b'x').string()
        with self.assertRaisesRegex(ValueError,'terminator'):
            Reader(n(1)+b'x').string()


if __name__ == '__main__':
    unittest.main()
