import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from lift_raw_spurs_job import fingerprint


class RawJobRegistrationTests(unittest.TestCase):
    def test_pinned_sdk_hash_vectors(self):
        # SDK's nonstandard offset basis is part of its registration ABI.
        self.assertEqual(fingerprint(b'abc'),0xe16801510db89efd)
        self.assertEqual(fingerprint(bytes(range(16))),0x088b84ce5271b343)

    def test_capture_mismatch_rejected_before_creating_lift(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            capture=root/'spujob_0000000000000000_16.bin'
            capture.write_bytes(bytes(16))
            output=root/'lift'
            result=subprocess.run([sys.executable,str(ROOT/'scripts/lift_raw_spurs_job.py'),str(capture),
                '--sdk',str(root/'absent-sdk'),'--out',str(output),'--base','0','--entry','0','--image-id','100'],
                capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('Capture filename fingerprint/size',result.stderr)
            self.assertFalse(output.exists())


if __name__=='__main__':
    unittest.main()
