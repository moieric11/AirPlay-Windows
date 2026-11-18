"""Check capture buffer lifetime and CUDA-only fallback without GPU hardware."""
import importlib.util
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from face_swap_worker import CapturedSwapper

HAS_NUMPY = importlib.util.find_spec("numpy") is not None


@unittest.skipUnless(HAS_NUMPY, "Run with the face-swap Python environment")
class CaptureTests(unittest.TestCase):
    def make_session(self, fail=False):
        import numpy as np
        allocations = []

        class Buffer:
            def __init__(self, value):
                self.value = value.copy()
                allocations.append(self)

            @classmethod
            def ortvalue_from_numpy(cls, array, device, ordinal):
                assert device == "cuda" and ordinal == 0
                return cls(array)

            @classmethod
            def ortvalue_from_shape_and_type(cls, shape, dtype, device, ordinal):
                assert device == "cuda" and ordinal == 0
                return cls(np.zeros(shape, dtype=dtype))

            def update_inplace(self, value):
                self.value[...] = value

        class Binding:
            def __init__(self):
                self.inputs, self.outputs = {}, {}

            def bind_ortvalue_input(self, name, value):
                self.inputs[name] = value

            def bind_ortvalue_output(self, name, value):
                self.outputs[name] = value

            def copy_outputs_to_cpu(self):
                return [value.value.copy() for value in self.outputs.values()]

        class Session:
            def __init__(self):
                self.configurations = []
                self.fallback_disabled = False

            def set_providers(self, providers):
                self.configurations.append(providers[0][1]["enable_cuda_graph"])

            def disable_fallback(self):
                self.fallback_disabled = True

            def get_providers(self):
                return ["CUDAExecutionProvider", "CPUExecutionProvider"]

            def get_outputs(self):
                return [SimpleNamespace(name="result", type="tensor(float)", shape=[1, 1, 1, 1])]

            def io_binding(self):
                return Binding()

            def run_with_iobinding(self, binding):
                if fail:
                    raise RuntimeError("Capture rejected by export")
                binding.outputs["result"].value[...] = sum(value.value.sum() for value in binding.inputs.values())

            def run(self, names, values):
                assert self.fallback_disabled
                return [np.full((1, 1, 1, 1), sum(value.sum() for value in values.values()), dtype=np.float32)]

        return Session(), SimpleNamespace(OrtValue=Buffer), allocations

    def test_input_updates_reuse_gpu_allocations(self):
        import numpy as np
        session, fake_ort, allocations = self.make_session()
        with patch.dict(sys.modules, {"onnxruntime": fake_ort}):
            runner = CapturedSwapper(session, 0)
            first = runner.run(["result"], {"image": np.array([1], dtype=np.float32), "identity": np.array([2], dtype=np.float32)})
            second = runner.run(["result"], {"image": np.array([5], dtype=np.float32), "identity": np.array([2], dtype=np.float32)})
            self.assertEqual(first[0].item(), 3)
            self.assertEqual(second[0].item(), 7)
            self.assertEqual(len(allocations), 3)
            self.assertEqual(len(runner.output_buffers), 1)
            self.assertTrue(runner.active)
            self.assertEqual(session.configurations, [1])

    def test_capture_failure_keeps_cuda_precision(self):
        import numpy as np
        session, fake_ort, _ = self.make_session(fail=True)
        with patch.dict(sys.modules, {"onnxruntime": fake_ort}):
            runner = CapturedSwapper(session, 0)
            result = runner.run(None, {"image": np.array([4], dtype=np.float32)})
            self.assertEqual(result[0].item(), 4)
            self.assertFalse(runner.active)
            self.assertEqual(session.configurations, [1, 0])
            self.assertIn("CUDAExecutionProvider", runner.get_providers())
            self.assertTrue(session.fallback_disabled)


if __name__ == "__main__":
    unittest.main()
