import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from audit_startup_tables import walk_ctor_table


class ConstructorTableTests(unittest.TestCase):
    def test_reverse_dispatch_order_and_opd(self):
        memory={0x1000:0xffffffff,0x1004:0x2000,0x1008:0x2010,
                0x2000:0x3000,0x2004:0x4000,0x2010:0x3010,0x2014:0x4000}
        rows,sentinel=walk_ctor_table(memory.__getitem__,0x100c)
        self.assertEqual(sentinel,0x1000)
        self.assertEqual([row['code'] for row in rows],['0x3010','0x3000'])
        self.assertEqual([row['toc'] for row in rows],['0x4000','0x4000'])

    def test_missing_sentinel_is_bounded(self):
        with self.assertRaisesRegex(ValueError,'sentinel not found'):
            walk_ctor_table(lambda address:0x2000,0x1000,limit=3)

    def test_null_entry_rejected(self):
        with self.assertRaisesRegex(ValueError,'Null OPD'):
            walk_ctor_table(lambda address:0,0x1000)


if __name__ == '__main__':
    unittest.main()
