import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from compat_source import resolve_local_includes, write_if_changed


class CompatSourceTests(unittest.TestCase):
    def test_relocated_source_keeps_local_and_external_include_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'local.h').write_text('/* header */')
            source = root / 'source.c'
            text = '#include "local.h"\n#include "generated.h"\n#include <stdint.h>\n'
            result = resolve_local_includes(text, source)
            self.assertIn(f'#include "{(root / "local.h").as_posix()}"', result)
            self.assertIn('#include "generated.h"', result)
            self.assertIn('#include <stdint.h>', result)

    def test_unchanged_output_preserves_timestamp_and_original_line_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'generated.c'
            target.write_bytes(b'first\r\nsecond\r\n')
            before = target.stat().st_mtime_ns
            self.assertFalse(write_if_changed(target, 'first\nsecond\n'))
            self.assertEqual(target.stat().st_mtime_ns, before)
            self.assertEqual(target.read_bytes(), b'first\r\nsecond\r\n')

    def test_new_and_changed_output_are_written(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'generated.c'
            self.assertTrue(write_if_changed(target, 'first\n'))
            self.assertTrue(write_if_changed(target, 'second\n'))
            self.assertEqual(target.read_text(), 'second\n')
