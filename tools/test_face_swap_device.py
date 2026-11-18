"""Device IO binding, graph replay and CUDA-only recovery without GPU hardware."""
import importlib.util
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from face_swap_gpu_worker import DeviceModel, make_restorer, make_detector
from pathlib import Path


@unittest.skipUnless(importlib.util.find_spec("numpy"), "Run with the face-swap Python environment")
class DeviceModelTests(unittest.TestCase):
    def fixture(self, fail_capture=False, fail_replay=False):
        import numpy as np
        sessions, allocations = [], []

        class Tensor:
            def __init__(self, shape):
                self.value = np.zeros(shape, dtype=np.float32)
                self.shape, self.data = shape, SimpleNamespace(ptr=len(allocations) + 1)
                allocations.append(self)

        class Binding:
            def bind_input(self, name, device, ordinal, dtype, shape, pointer):
                self.input = allocations[pointer - 1]

            def bind_output(self, name, device, ordinal, dtype, shape, pointer):
                self.output = allocations[pointer - 1]

        class Options:
            def add_session_config_entry(self, key, value):
                assert (key, value) == ("session.disable_cpu_ep_fallback", "1")

        class Session:
            def __init__(self, path, sess_options, providers):
                self.provider = providers[0][1]
                assert len(providers) == 1 and providers[0][0] == "CUDAExecutionProvider"
                self.calls = 0
                sessions.append(self)

            def disable_fallback(self):
                self.disabled = True

            def get_providers(self):
                return ["CUDAExecutionProvider"]

            def get_inputs(self):
                return [SimpleNamespace(name="image", shape=[1], type="tensor(float)")]

            def get_outputs(self):
                return [SimpleNamespace(name="result", shape=[1], type="tensor(float)")]

            def io_binding(self):
                return Binding()

            def run_with_iobinding(self, binding):
                self.calls += 1
                if self.provider["enable_cuda_graph"] and (fail_capture or (fail_replay and self.calls > 1)):
                    raise RuntimeError("Graph execution failed")
                binding.output.value[...] = binding.input.value * 2

        cp = SimpleNamespace(zeros=lambda shape, dtype: Tensor(shape), float32=np.float32)
        ort = SimpleNamespace(InferenceSession=Session, SessionOptions=Options)
        stream = SimpleNamespace(ptr=987, synchronize=lambda: None)
        return cp, ort, stream, sessions, allocations

    def test_replay_uses_updated_input_and_stable_addresses(self):
        cp, ort, stream, sessions, allocations = self.fixture()
        with patch.dict(sys.modules, {"onnxruntime": ort}):
            model = DeviceModel(cp, "model.onnx", stream, 2, cuda_graph=True)
            model.inputs[0].value[...] = 3
            self.assertEqual(model.run()[0].value.item(), 6)
            model.inputs[0].value[...] = 7
            self.assertEqual(model.run()[0].value.item(), 14)
        self.assertTrue(model.graph_ready)
        self.assertEqual(len(allocations), 2)
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0].provider["user_compute_stream"], "987")

    def test_capture_failure_rebinds_existing_buffers_to_cuda(self):
        cp, ort, stream, sessions, allocations = self.fixture(fail_capture=True)
        with patch.dict(sys.modules, {"onnxruntime": ort}):
            model = DeviceModel(cp, "model.onnx", stream, 2, cuda_graph=True)
            model.inputs[0].value[...] = 5
            self.assertEqual(model.run()[0].value.item(), 10)
            model.inputs[0].value[...] = 8
            self.assertEqual(model.run()[0].value.item(), 16)
        self.assertFalse(model.graph_enabled)
        self.assertEqual(len(allocations), 2)
        self.assertEqual([s.provider["enable_cuda_graph"] for s in sessions], [1, 0])
        self.assertTrue(all(s.disabled and s.provider["device_id"] == 2 for s in sessions))

    def test_replay_error_is_propagated_for_pipeline_recovery(self):
        cp, ort, stream, sessions, _ = self.fixture(fail_replay=True)
        with patch.dict(sys.modules, {"onnxruntime": ort}):
            model = DeviceModel(cp, "model.onnx", stream, 0, cuda_graph=True)
            model.run()
            with self.assertRaises(RuntimeError):
                model.run()
        self.assertEqual(len(sessions), 1)

    def test_tensorrt_restoration_warms_fp32_without_replacing_cuda_models(self):
        from unittest.mock import Mock
        for side in (256, 512):
            with self.subTest(side=side):
                trt = Mock()
                with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
                    with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                        model, backend = make_restorer(None, Path(f"GPEN-BFR-{side}.onnx"), None, 0, Path("cache/kernels"), True)
                self.assertIs(model, trt.return_value)
                self.assertEqual(backend, "TensorRT FP32")
                trt.return_value.run.assert_called_once()
                self.assertFalse(trt.call_args.kwargs["fp16"])
                cuda.assert_not_called()

    def test_tensorrt_build_failure_keeps_gpu_restoration(self):
        from unittest.mock import Mock
        for side in (256, 512):
            with self.subTest(side=side):
                trt = Mock(side_effect=RuntimeError("Runtime missing or build rejected"))
                with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
                    with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                        model, backend = make_restorer(None, Path(f"GPEN-BFR-{side}.onnx"), None, 0, Path("cache/kernels"), True)
                self.assertIs(model, cuda.return_value)
                self.assertEqual(backend, "CUDA")
                cuda.assert_called_once()

    def test_other_restoration_models_keep_cuda(self):
        with patch("face_swap_gpu_worker.DeviceModel") as cuda:
            model, backend = make_restorer(None, Path("other-restorer.onnx"), None, 0, Path("cache/kernels"), True)
        self.assertIs(model, cuda.return_value)
        self.assertEqual(backend, "CUDA")

    def test_detector_tensorrt_preserves_static_graph_and_reuses_prepared_file(self):
        from unittest.mock import Mock
        import tempfile
        graph = b"fixed-detector-graph"
        with tempfile.TemporaryDirectory() as folder:
            trt = Mock()
            with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
                with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                    model, backend = make_detector(None, graph, None, 2, Path(folder)/"kernels", [(3, 1)], True)
                    self.assertIs(model, trt.return_value)
                    self.assertEqual(backend, "TensorRT FP32")
                    self.assertFalse(trt.call_args.kwargs["fp16"])
                    path = trt.call_args.args[1]
                    self.assertEqual(path.read_bytes(), graph)
                    stamp = path.stat().st_mtime_ns
                    make_detector(None, graph, None, 2, Path(folder)/"kernels", [(3, 1)], True)
                    self.assertEqual(path.stat().st_mtime_ns, stamp)
                    cuda.assert_not_called()
                    self.assertEqual(trt.return_value.run.call_count, 2)

    def test_detector_build_or_warmup_failure_keeps_cuda_binding(self):
        from unittest.mock import Mock
        import tempfile
        for fail_at in ("build", "warmup"):
            with self.subTest(fail_at=fail_at), tempfile.TemporaryDirectory() as folder:
                trt = Mock()
                if fail_at == "build":
                    trt.side_effect = RuntimeError("TensorRT missing")
                else:
                    trt.return_value.run.side_effect = RuntimeError("Rejected detector")
                with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
                    with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                        model, backend = make_detector(None, b"graph", None, 0, Path(folder)/"kernels", [(3, 1)], True)
                self.assertIs(model, cuda.return_value)
                self.assertEqual(backend, "CUDA")
                cuda.assert_called_once_with(None, b"graph", None, 0,
                    output_shapes=[(3, 1)], input_shapes=[(1, 3, 320, 320)])

    def test_detector_without_tensorrt_does_not_prepare_files(self):
        from unittest.mock import Mock
        trt = Mock()
        with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
            with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                model, backend = make_detector(None, b"graph", None, 0, Path("unused/kernels"), [(3, 1)], False)
        self.assertIs(model, cuda.return_value)
        self.assertEqual(backend, "CUDA")
        trt.assert_not_called()


if __name__ == "__main__":
    unittest.main()
