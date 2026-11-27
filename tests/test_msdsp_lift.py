import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from lift_msdsp import lift_module


class DspModuleTests(unittest.TestCase):
    def test_branch_to_another_plugin_reenters_dispatch(self):
        sdk = ROOT / 'third_party' / 'ps3recomp'
        caller, target = 0x37000, 0x39000
        branch = 0x32000000 | ((target - caller) // 4 << 7)  # SPU br target
        return_to_caller = struct.pack('>I', 0x35000000)  # SPU bi r0
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / 'caller', Path(tmp) / 'target'
            lift_module(sdk, first, caller, struct.pack('>I', branch), [caller])
            lift_module(sdk, second, target, return_to_caller, [target])
            caller_code = (first / 'spu_recomp.c').read_text(encoding='utf-8')
            target_code = (second / 'spu_recomp.c').read_text(encoding='utf-8')

        # The caller's external stub returns the destination PC to the normal
        # resolver. Only the owning plugin registers an implementation there.
        self.assertIn('ctx->pc = 0x39000u; g_spu_trampoline_fn = spu_indirect_branch;', caller_code)
        self.assertNotIn('{ 0x00039000u,', caller_code)
        self.assertIn('{ 0x00039000u, port_msdsp_39000_spu_func_00039000,', target_code)
