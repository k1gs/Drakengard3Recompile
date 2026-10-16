import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from prepare_ppu_vmx import prepare


class VmxPatchTests(unittest.TestCase):
    def test_guard_replacement_preserves_other_bytes_and_is_idempotent(self):
        raw=b'#include "ppu_recomp.h"\n// legacy \x97\nport_unimplemented_instruction(0x151A624ULL, "vpkswss v0, v0, v1");\nport_unimplemented_instruction(0x123ULL, "unknown");\n'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);path=root/'ppu_recomp_000.cpp';path.write_bytes(raw)
            self.assertEqual(prepare(root),(1,1))
            changed=path.read_bytes()
            self.assertIn(b'port_vpkswss(ctx,0,0,1);',changed)
            self.assertIn(b'// legacy \x97',changed)
            self.assertIn(b'"unknown"',changed)
            self.assertEqual((root/'compat-before-vpkswss'/path.name).read_bytes(),raw)
            self.assertEqual(prepare(root),(0,1))
            self.assertEqual(path.read_bytes(),changed)
