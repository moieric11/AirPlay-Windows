import tempfile
import unittest
from pathlib import Path

import face_swap_progress as progress


class PreparationProgressTests(unittest.TestCase):
    def tearDown(self):
        progress.configure(None)

    def test_stage_replaces_previous_stage_beside_private_ready_file(self):
        with tempfile.TemporaryDirectory() as directory:
            ready = Path(directory) / "worker.port"
            progress.configure(ready)
            progress.report("Detector")
            progress.report("TensorRT: building swapper\nlevel 5")
            self.assertEqual(ready.with_suffix(".progress").read_text(),
                             "TensorRT: building swapper level 5")
            self.assertFalse(ready.exists())
            self.assertFalse(ready.with_suffix(".progress.tmp").exists())

    def test_missing_or_unwritable_diagnostics_does_not_break_engine(self):
        progress.configure(None)
        progress.report("Disabled")
        with tempfile.TemporaryDirectory() as directory:
            progress.configure(Path(directory) / "missing" / "worker.port")
            progress.report("Swapper")


if __name__ == "__main__":
    unittest.main()
