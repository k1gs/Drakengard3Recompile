import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath

PROJECT = Path(__file__).resolve().parents[1]
SDK = PROJECT / 'third_party' / 'ps3recomp'
sys.path.insert(0, str(PROJECT / 'scripts'))
from compat_source import resolve_local_includes, write_if_changed


class CompatSourceTests(unittest.TestCase):
    def test_relocated_source_keeps_local_and_external_include_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ('sdk', 'relocated SDK'):
                sdk = Path(tmp) / name
                module = sdk / 'runtime' / 'module'
                module.mkdir(parents=True)
                (module / 'local.h').touch()
                (module.parent / 'shared.h').touch()
                text = ('#include "local.h" // beside the SDK source\n'
                        '  # include "../shared.h"\n'
                        '#include "generated.h"\n#include <stdint.h>\n')
                expected = ('#include "runtime/module/local.h" // beside the SDK source\n'
                            '  # include "runtime/shared.h"\n'
                            '#include "generated.h"\n#include <stdint.h>\n')
                self.assertEqual(resolve_local_includes(text, module / 'source.c', sdk), expected)

    @unittest.skipUnless((SDK / 'runtime/ppu/ppu_sysprx.cpp').is_file(), 'requires the project SDK')
    def test_prepared_sources_have_portable_include_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            subprocess.run([sys.executable, PROJECT / 'scripts/prepare_runtime.py',
                            '--sdk', SDK, '--out', output / 'ppu_sysprx.cpp'],
                           cwd=output, check=True, stdout=subprocess.DEVNULL)
            for source in output.iterdir():
                text = source.read_text(encoding='utf-8')
                for header in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, re.MULTILINE):
                    with self.subTest(source=source.name, header=header):
                        self.assertFalse(PureWindowsPath(header).anchor)
            memory = (output / 'sys_memory_compat.c').read_text(encoding='utf-8')
            self.assertIn('#include "sys_memory_compat.h"', memory)

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
