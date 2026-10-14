import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from namespace_spu_registry import namespace


class SpuNamespaceTests(unittest.TestCase):
    def fixture(self):
        return '\n'.join(
            f'spu_begin_image({i});\n'
            f'spu_workload_register_img(0x{i:X}ULL, entry_{i}, {i}, "image_{i}");'
            for i in range(1, 41)
        ) + '\nspu_begin_image(0);'

    def test_preserves_sentinel_and_fingerprints(self):
        result = namespace(self.fixture(), 40)
        self.assertIn('spu_begin_image(0);', result)
        self.assertIn('spu_begin_image(41);', result)
        self.assertIn('spu_begin_image(80);', result)
        self.assertIn('0x1ULL, entry_1, 41, "image_1"', result)
        self.assertIn('0x28ULL, entry_40, 80, "image_40"', result)

    def test_rejects_changed_registry_or_collision(self):
        with self.assertRaises(ValueError):
            namespace(self.fixture().replace('spu_begin_image(20);', ''), 40)
        with self.assertRaises(ValueError):
            namespace(self.fixture(), 60)
        with self.assertRaises(ValueError):
            namespace(self.fixture().replace('0x1ULL, entry_1, 1,', '0x1ULL, entry_1, 0,'), 40)


if __name__ == '__main__':
    unittest.main()
