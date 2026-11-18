"""Opt-in real CUDA checks for fused SCRFD metadata decoding.

Run with FACE_SWAP_GPU_TESTS=1 in the CUDA face-swap environment.
"""
import os
from pathlib import Path
import unittest


@unittest.skipUnless(os.environ.get("FACE_SWAP_GPU_TESTS") == "1", "CUDA hardware test not selected")
class DetectionMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from face_swap_gpu_worker import load_cupy
        cls.cp = load_cupy(Path("build/cupy-cache").resolve())

    def test_all_anchor_layouts_thresholds_and_updated_buffers(self):
        import numpy as np
        from face_swap_detection import DetectionMetadata
        cp = self.cp
        for strides, anchors in (([8, 16, 32], 2), ([8, 16, 32], 1), ([8, 16, 32, 64, 128], 1)):
            counts = [(320//stride)**2*anchors for stride in strides]
            rng = np.random.default_rng(719)
            scores = [rng.random((count, 1), dtype=np.float32) for count in counts]
            boxes = [rng.random((count, 4), dtype=np.float32)*20 for count in counts]
            points = [rng.standard_normal((count, 10)).astype(np.float32)*10 for count in counts]
            values = scores+boxes+points
            stream = cp.cuda.Stream(non_blocking=True)
            with stream:
                outputs = [cp.asarray(value) for value in values]
                decoded = DetectionMetadata(cp, outputs, strides, anchors)
                addresses = [value.data.ptr for value in outputs]
                for mode in ("mixed", "all", "none", "threshold", "mixed"):
                    for index, score in enumerate(scores):
                        score[:] = (rng.random(score.shape, dtype=np.float32) if mode == "mixed" else
                                    1 if mode == "all" else 0 if mode == "none" else .5)
                        if mode == "threshold":
                            score[0] = np.nextafter(np.float32(.5), np.float32(0))
                            score[-1] = np.nan
                        outputs[index].set(score, stream=stream)
                    expected = []
                    for index, (count, stride) in enumerate(zip(counts, strides)):
                        centers = np.stack(np.meshgrid(np.arange(320//stride, dtype=np.float32),
                            np.arange(320//stride, dtype=np.float32)), axis=-1).reshape(-1, 2)
                        centers = np.repeat(centers, anchors, axis=0)*stride
                        keep = np.flatnonzero(scores[index].reshape(-1) >= .5)
                        center = centers[keep]
                        distances = boxes[index][keep]*stride
                        landmarks = points[index][keep].reshape(-1, 5, 2)*stride+center[:, None, :]
                        expected.append(np.concatenate((center-distances[:, :2], center+distances[:, 2:],
                            scores[index][keep], landmarks.reshape(-1, 10)), axis=1))
                    actual = decoded.download(stream)
                    np.testing.assert_array_equal(actual, np.concatenate(expected))
                    self.assertEqual([value.data.ptr for value in outputs], addresses)
                    self.assertEqual(actual.shape[0], sum(counts) if mode == "all" else actual.shape[0])


if __name__ == "__main__":
    unittest.main()
