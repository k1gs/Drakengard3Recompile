import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare_renderer import prepare as prepare_renderer


class PipelineStoragePreparationTests(unittest.TestCase):
    def test_cmake_tracks_pipeline_storage_preparation_script(self):
        cmake = (ROOT / 'port/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('prepare_pipeline_storage.py', cmake)

    def test_renderer_preparation_is_deterministic_and_embeds_bounded_fp_cache(self):
        sdk = ROOT / 'third_party/ps3recomp'
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            prepare_renderer(sdk, Path(a))
            prepare_renderer(sdk, Path(b))
            first = (Path(a) / 'rsx_d3d12_backend_compat.c').read_text(encoding='utf-8')
            second = (Path(b) / 'rsx_d3d12_backend_compat.c').read_text(encoding='utf-8')
            self.assertEqual(first, second)
            self.assertIn('#define PORT_FP_BYTECODE_SLOTS 4096u', first)
            self.assertIn('#define PORT_FP_BYTECODE_MAX_TOTAL_BYTES (48u * 1024u * 1024u)', first)
            self.assertIn('#define PORT_FP_HISTORY_MAX_TOTAL_BYTES (16u * 1024u * 1024u)', first)
            self.assertIn('cpu=%.3fs parallel=%u first=%.3fs/%u recompile=%.3fs/%u', first)
            self.assertIn('port_prewarm_batch_fp();', first)
