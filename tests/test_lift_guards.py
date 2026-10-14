import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from guard_generated_lift import guard


class LiftGuardTests(unittest.TestCase):
    def test_unknown_instruction_and_taken_branch_preserve_source(self):
        raw = (b'// comment with legacy byte \x97\n'
               b'void func_00010000(ppu_context* ctx) {\n'
               b'        /* TODO: .word 0x00000000 */;\n'
               b'        if (condition) return; /* bc -> non-code 0xFFFFFFFC */\n}\n')
        name = 'ppu_recomp_000.cpp'
        audit = {'chunks': [{'name': name, 'sha256': hashlib.sha256(raw).hexdigest()}],
                 'findings': [
                     {'file': name, 'line': 3, 'function': 'func_00010000', 'kind': 'todo', 'detail': '.word 0x00000000'},
                     {'file': name, 'line': 4, 'function': 'func_00010000', 'kind': 'discarded-or-unmodeled', 'detail': 'if (condition) return; /* bc -> non-code 0xFFFFFFFC */'}]}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/name
            path.write_bytes(raw)
            self.assertEqual(guard(Path(tmp), audit), (1, 2))
            changed = path.read_bytes()
            self.assertIn(b'if (condition) port_unimplemented_instruction(', changed)
            self.assertNotIn(b'/* TODO:', changed)
            self.assertIn(b'legacy byte \x97', changed)
            self.assertEqual((Path(tmp)/'original-unguarded'/name).read_bytes(), raw)

    def test_changed_chunk_is_rejected_before_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'ppu_recomp_000.cpp'
            path.write_bytes(b'changed')
            audit = {'chunks': [{'name': path.name, 'sha256': '0'*64}],
                     'findings': [{'file': path.name}]}
            with self.assertRaisesRegex(ValueError, 'changed since audit'):
                guard(Path(tmp), audit)
            self.assertEqual(path.read_bytes(), b'changed')
            self.assertFalse((Path(tmp)/'original-unguarded').exists())


if __name__ == '__main__':
    unittest.main()
