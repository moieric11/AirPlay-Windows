"""Protect identity conventions, normalization and AlphaFace shape repair."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch
import sys

from face_swap_models import identity_tensor, image_convention, prepare_model


@unittest.skipUnless(importlib.util.find_spec("numpy"), "Face-swap environment required")
class SwapperAdapterTests(unittest.TestCase):
    def test_facefusion_alignment_recovers_known_geometry_and_preserves_legacy(self):
        import cv2
        import numpy as np
        from face_swap_models import estimate_face_matrix
        template=np.array([[.36167656,.40387734],[.63696719,.40235469],
                           [.50019687,.56044219],[.38710391,.72160547],
                           [.61507734,.72034453]])
        for side in (128,256,512):
            matrix=np.array([[.8,-.2,12],[.2,.8,-8]],np.float64)
            points=cv2.transform((template*side)[None],cv2.invertAffineTransform(matrix))[0].astype(np.float32)
            np.testing.assert_allclose(estimate_face_matrix(points,side,1),matrix,atol=2e-5)
            from insightface.utils.face_align import estimate_norm
            np.testing.assert_array_equal(estimate_face_matrix(points,side,0),estimate_norm(points,image_size=side))
        for points in (np.zeros((5,2),np.float32),np.full((5,2),np.nan,np.float32)):
            with self.assertRaises(ValueError):estimate_face_matrix(points,256,1)
        with self.assertRaises(ValueError):estimate_face_matrix(np.zeros((5,2)),256,2)

    def test_identity_vectors_are_model_specific(self):
        import numpy as np
        source = SimpleNamespace(embedding=np.array([3., 4.], dtype=np.float32),
                                 normed_embedding=np.array([.6, .8], dtype=np.float32))
        original = SimpleNamespace(emap=np.array([[1., 0.], [0., 2.]], dtype=np.float32))
        np.testing.assert_array_equal(identity_tensor("alphaface_256", source), [[3, 4]])
        for version in ("a", "b", "c"):
            np.testing.assert_allclose(identity_tensor(f"hyperswap_1{version}_256", source), [[.6, .8]])
        expected = np.array([[.6, 1.6]], dtype=np.float32)
        expected /= np.linalg.norm(expected)
        np.testing.assert_allclose(identity_tensor("inswapper_128", source, original), expected)
        with self.assertRaises(ValueError):
            identity_tensor("unknown", source)

    def test_normalization_matches_output_byte_scale(self):
        import numpy as np
        pixels = np.array([0., 64., 127.5, 255.], dtype=np.float32)
        for model in ("inswapper_128", "alphaface_256", "hyperswap_1a_256"):
            mean, scale, out_scale, bias = image_convention(model)
            normalized = (pixels-mean)*scale
            np.testing.assert_allclose(normalized*out_scale+bias, pixels, atol=2e-5)

    def test_tensor_rt_failure_preserves_selected_swapper(self):
        from face_swap_gpu_worker import make_swapper
        base = SimpleNamespace(swapper_backend="tensorrt_fp32",
                               swapper=SimpleNamespace(model_file="hyperswap_1c_256.onnx"))
        trt = Mock(side_effect=RuntimeError("TensorRT unavailable"))
        with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
            with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                model, backend = make_swapper(None, base, None, 0, Path("cache/kernels"))
        self.assertIs(model, cuda.return_value)
        self.assertEqual(backend, "CUDA")
        self.assertEqual(cuda.call_args.args[1], "hyperswap_1c_256.onnx")

    def test_native_tensor_rt_uses_fp32_and_warms_bound_model(self):
        from face_swap_gpu_worker import make_swapper
        base = SimpleNamespace(swapper_backend="tensorrt_fp32",
                               swapper=SimpleNamespace(model_file="alphaface_256.onnx"))
        trt = Mock()
        with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
            model, backend = make_swapper(None, base, None, 0, Path("cache/kernels"))
        self.assertIs(model, trt.return_value)
        self.assertEqual(backend, "TensorRT FP32")
        self.assertFalse(trt.call_args.kwargs["fp16"])
        self.assertEqual(trt.call_args.kwargs["builder_level"], 5)
        model.run.assert_called_once()

    def test_hyperswap_b_uses_measured_level5_without_changing_precision(self):
        from face_swap_gpu_worker import make_swapper
        base = SimpleNamespace(swapper_backend="tensorrt_fp32",
                               swapper=SimpleNamespace(model_file="hyperswap_1b_256.onnx"))
        trt = Mock()
        with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
            model, backend = make_swapper(None, base, None, 0, Path("cache/kernels"))
        self.assertIs(model, trt.return_value)
        self.assertEqual(backend, "TensorRT FP32")
        self.assertEqual(trt.call_args.kwargs["builder_level"], 5)
        self.assertFalse(trt.call_args.kwargs["fp16"])

    def test_level5_failure_keeps_cuda_without_building_a_lower_level(self):
        from face_swap_gpu_worker import make_swapper
        base = SimpleNamespace(swapper_backend="tensorrt_fp32",
                               swapper=SimpleNamespace(model_file="hyperswap_1b_256.onnx"))
        bad, good = Mock(), Mock()
        bad.run.side_effect = RuntimeError("Level 5 warmup rejected")
        trt = Mock(side_effect=[bad, good])
        with patch.dict(sys.modules, {"face_swap_tensorrt": SimpleNamespace(TensorRTModel=trt)}):
            with patch("face_swap_gpu_worker.DeviceModel") as cuda:
                model, backend = make_swapper(None, base, None, 0, Path("cache/kernels"))
        self.assertIs(model, cuda.return_value)
        self.assertEqual(backend, "CUDA")
        self.assertEqual([c.kwargs["builder_level"] for c in trt.call_args_list], [5])
        self.assertTrue(all(not c.kwargs["fp16"] for c in trt.call_args_list))
        cuda.assert_called_once()

    def test_explicit_builder_choice_overrides_automatic_model_policy(self):
        from face_swap_gpu_worker import make_swapper
        for name, choice in (("hyperswap_1b_256",3),("hyperswap_1c_256",5),("alphaface_256",5)):
            with self.subTest(name=name,choice=choice):
                base=SimpleNamespace(swapper_backend="tensorrt_fp32",swapper_builder=choice,
                                     swapper=SimpleNamespace(model_file=name+".onnx"))
                trt=Mock()
                with patch.dict(sys.modules,{"face_swap_tensorrt":SimpleNamespace(TensorRTModel=trt)}):
                    make_swapper(None,base,None,0,Path("cache/kernels"))
                self.assertEqual(trt.call_args.kwargs["builder_level"],choice)
                self.assertFalse(trt.call_args.kwargs["fp16"])

    def test_invalid_builder_selection_cannot_silently_fall_back(self):
        from face_swap_gpu_worker import make_swapper
        base=SimpleNamespace(swapper_backend="tensorrt_fp32",swapper_builder=4,
                             swapper=SimpleNamespace(model_file="hyperswap_1b_256.onnx"))
        with self.assertRaises(ValueError):
            make_swapper(None,base,None,0,Path("cache/kernels"))

    @unittest.skipUnless(importlib.util.find_spec("onnx"), "ONNX required")
    def test_directml_hyperswap_promotes_internal_math_and_caches_separately(self):
        import numpy as np
        import onnx
        import onnxruntime as ort
        from onnx import helper, numpy_helper, TensorProto
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            original = root / "hyper.onnx"
            model = helper.make_model(helper.make_graph([
                helper.make_node("Cast", ["input"], ["half"], to=TensorProto.FLOAT16),
                helper.make_node("Constant", [], ["bias"], value=numpy_helper.from_array(np.array([.25], np.float16))),
                helper.make_node("Mul", ["half", "weight"], ["scaled"]),
                helper.make_node("Add", ["scaled", "bias"], ["sum"]),
                helper.make_node("Cast", ["sum"], ["output"], to=TensorProto.FLOAT),
            ], "half-math", [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1])],
                [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1])],
                [numpy_helper.from_array(np.array([.5], np.float16), "weight")],
                value_info=[helper.make_tensor_value_info("half", TensorProto.FLOAT16, [1])]),
                opset_imports=[helper.make_opsetid("", 17)])
            model.ir_version = 9
            onnx.save(model, original)
            before = original.read_bytes()
            for name in ("hyperswap_1a_256", "hyperswap_1b_256", "hyperswap_1c_256"):
                for backend in ("cuda", "cpu"):
                    self.assertEqual(prepare_model(name, original, root / "cache", backend), str(original))
                prepared = Path(prepare_model(name, original, root / "cache", "directml"))
                promoted = onnx.load(prepared)
                self.assertEqual(promoted.graph.initializer[0].data_type, TensorProto.FLOAT)
                self.assertEqual(promoted.graph.value_info[0].type.tensor_type.elem_type, TensorProto.FLOAT)
                self.assertEqual(promoted.graph.node[0].attribute[0].i, TensorProto.FLOAT)
                self.assertEqual(promoted.graph.node[1].attribute[0].t.data_type, TensorProto.FLOAT)
                session = ort.InferenceSession(str(prepared), providers=["CPUExecutionProvider"])
                np.testing.assert_allclose(session.run(None, {"input": np.array([.333333], np.float32)})[0],
                                           [.333333*.5+.25], atol=1e-7)
                stamp = prepared.stat().st_mtime_ns
                self.assertEqual(prepare_model(name, original, root / "cache", "directml"), str(prepared))
                self.assertEqual(prepared.stat().st_mtime_ns, stamp)
            self.assertEqual(original.read_bytes(), before)
            model.graph.initializer[0].CopyFrom(numpy_helper.from_array(np.array([.75], np.float16), "weight"))
            onnx.save(model, original)
            self.assertNotEqual(prepare_model(name, original, root / "cache", "directml"), str(prepared))

    @unittest.skipUnless(importlib.util.find_spec("onnx"), "ONNX required")
    def test_shape_specialization_keeps_original_and_reuses_cache(self):
        import onnx
        from onnx import helper, TensorProto
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            target = helper.make_tensor_value_info("target", TensorProto.FLOAT, ["batch", 3, 256, 256])
            source = helper.make_tensor_value_info("source", TensorProto.FLOAT, ["batch", 512])
            output = helper.make_tensor_value_info("output", TensorProto.FLOAT, ["batch", 3, "batch", "width"])
            graph = helper.make_graph([helper.make_node("Identity", ["target"], ["output"])], "fixture", [target, source], [output])
            original = root / "alphaface.onnx"
            onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]), original)
            before = original.read_bytes()
            result = Path(prepare_model("alphaface_256", original, root / "cache"))
            self.assertEqual(original.read_bytes(), before)
            repaired = onnx.load(str(result))
            self.assertEqual([d.dim_value for d in repaired.graph.output[0].type.tensor_type.shape.dim], [1, 3, 256, 256])
            modified = result.stat().st_mtime_ns
            self.assertEqual(prepare_model("alphaface_256", original, root / "cache"), str(result))
            self.assertEqual(result.stat().st_mtime_ns, modified)


if __name__ == "__main__":
    unittest.main()
