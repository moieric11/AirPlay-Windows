"""Builder experiments must not reuse the level-3 cache or alter precision."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from face_swap_tensorrt import TensorRTModel


class BuilderTests(unittest.TestCase):
    def test_selected_level_reaches_every_tensor_rt_model_factory(self):
        from face_swap_gpu_worker import make_detector, make_restorer, make_swapper
        from face_swap_occlusion import OcclusionMask
        from face_swap_parser import RegionMask
        from face_swap_detectors import GpuDetector
        from face_swap_landmarker import GpuLandmarker
        class ReachedTensorRT(BaseException):pass
        # Stop at the real TensorRT boundary before touching GPU hardware.
        for level in (3,5):
            with tempfile.TemporaryDirectory() as folder:
                cache=Path(folder)/'cuda-kernels'
                source=SimpleNamespace(kind='yoloface',side=640,graph_path=Path('yoloface.onnx'))
                base=SimpleNamespace(swapper_backend='tensorrt_fp32',swapper_builder=level,
                                     swapper=SimpleNamespace(model_file='hyperswap_1a_256.onnx'))
                factories=[
                    ('SCRFD',lambda:make_detector(None,b'graph',None,0,cache,[],True,builder_level=level)),
                    ('GPEN256',lambda:make_restorer(None,Path('GPEN-BFR-256.onnx'),None,0,cache,True,builder_level=level)),
                    ('GPEN512',lambda:make_restorer(None,Path('GPEN-BFR-512.onnx'),None,0,cache,True,builder_level=level)),
                    ('XSeg',lambda:OcclusionMask(None,Path('xseg.onnx'),None,0,cache,(256,),builder_level=level)),
                    ('BiSeNet',lambda:RegionMask(None,Path('bisenet.onnx'),None,0,cache,(256,),builder_level=level)),
                    ('YOLOFace',lambda:GpuDetector(None,source,None,0,cache,True,builder_level=level)),
                    ('RetinaFace',lambda:GpuDetector(None,SimpleNamespace(kind='retinaface',side=320,graph_path=Path('retina.onnx')),None,0,cache,True,builder_level=level)),
                    ('YuNet',lambda:GpuDetector(None,SimpleNamespace(kind='yunet',side=640,graph_path=Path('yunet.onnx')),None,0,cache,True,builder_level=level)),
                    ('Swapper',lambda:make_swapper(None,base,None,0,cache)),
                    ('HRFFA',lambda:GpuLandmarker(None,Path('hrffa.onnx'),None,0,cache,builder_level=level))]
                for name,factory in factories:
                    with self.subTest(level=level,model=name), \
                         patch('face_swap_tensorrt.TensorRTModel',side_effect=ReachedTensorRT) as trt, \
                         patch('face_swap_occlusion.static_model',return_value=Path('xseg-prepared.onnx')), \
                         patch('face_swap_parser.static_parser',return_value=Path('bisenet-prepared.onnx')), \
                         patch('face_swap_landmarker.static_landmarker',return_value=Path('hrffa-prepared.onnx')):
                        with self.assertRaises(ReachedTensorRT):factory()
                        trt.assert_called_once()
                        self.assertEqual(trt.call_args.kwargs['builder_level'],level)
                        self.assertFalse(trt.call_args.kwargs['fp16'])

    def test_builder_levels_have_separate_caches_and_preserve_default_key(self):
        cp=SimpleNamespace(cuda=SimpleNamespace(runtime=SimpleNamespace(
            getDeviceProperties=lambda _: {"name":b"test GPU","major":8,"minor":6},
            driverGetVersion=lambda:13000)))
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);model=root/"model.onnx";model.write_bytes(b"fixture")
            with patch.dict(sys.modules,{"onnxruntime":SimpleNamespace(__version__="test-ort")}):
                with patch("face_swap_tensorrt.importlib.metadata.version",return_value="test-trt"), \
                     patch("face_swap_tensorrt.load_tensorrt_libraries",return_value=[]), \
                     patch("face_swap_gpu_worker.DeviceModel.__init__",return_value=None):
                    default=TensorRTModel(cp,model,None,0,root/"cache")
                    level3=TensorRTModel(cp,model,None,0,root/"cache",builder_level=3)
                    level5=TensorRTModel(cp,model,None,0,root/"cache",builder_level=5)
            fingerprint={"model":hashlib.sha256(b"fixture").hexdigest(),"ort":"test-ort","trt":"test-trt",
                "gpu":"test GPU","compute":[8,6],"driver":13000,"fp16":False,"builder":3}
            key=hashlib.sha256(json.dumps(fingerprint,sort_keys=True).encode()).hexdigest()[:24]
            self.assertEqual(default.cache,level3.cache)
            self.assertEqual(default.cache.name,"model-"+key)
            self.assertNotEqual(level3.cache,level5.cache)
            manifest=json.loads((level5.cache/"manifest.json").read_text())
            self.assertEqual(manifest["builder"],5)
            self.assertFalse(manifest["fp16"])

    def test_session_receives_selected_level_with_unchanged_precision_and_workspace(self):
        options=SimpleNamespace(add_session_config_entry=Mock())
        session=SimpleNamespace(get_providers=lambda:["TensorrtExecutionProvider","CUDAExecutionProvider"],disable_fallback=Mock())
        factory=Mock(return_value=session)
        model=TensorRTModel.__new__(TensorRTModel)
        model.ort=SimpleNamespace(SessionOptions=lambda:options,InferenceSession=factory)
        model.path="model.onnx";model.device=0;model.stream=SimpleNamespace(ptr=123)
        model.cache=Path("cache");model.profile=False;model.fp16=False;model.builder_level=5
        self.assertIs(model.create_session(),session)
        providers=factory.call_args.kwargs["providers"]
        settings=providers[0][1]
        self.assertEqual(settings["trt_builder_optimization_level"],5)
        self.assertEqual(settings["trt_max_workspace_size"],2*1024**3)
        self.assertFalse(settings["trt_fp16_enable"])
        self.assertEqual(settings["trt_auxiliary_streams"],0)
        self.assertEqual([p[0] for p in providers],["TensorrtExecutionProvider","CUDAExecutionProvider"])
        session.disable_fallback.assert_called_once()

    def test_invalid_builder_level_is_rejected_before_gpu_initialization(self):
        for value in (-1,6,3.5,True,None):
            with self.subTest(value=value),self.assertRaises(ValueError):
                TensorRTModel(None,None,None,0,None,builder_level=value)


if __name__=="__main__":
    unittest.main()
