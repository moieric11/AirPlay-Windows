import os
from pathlib import Path
import tempfile
import unittest
import numpy as np
from face_swap_landmarker import crop_matrix,five_points,decode_prediction,static_landmarker


class LandmarkerTests(unittest.TestCase):
    def test_generated_portrait_can_move_away_from_static_photo_points(self):
        raw,box,world,detected=self.fixture()
        moved=raw.copy();moved[...,0]+=.25
        self.assertIsNone(decode_prediction(moved,box,detected))
        result=decode_prediction(moved,box,detected,check_reference=False)
        self.assertIsNotNone(result)
        self.assertGreater(float(np.max(result[:,0]-world[:,0])),.35*np.max(box[2:]-box[:2]))
        self.assertIsNone(decode_prediction(raw+10,box,detected,check_reference=False))

    def fixture(self):
        points=np.tile([.5,.5],(68,1)).astype(np.float32)
        points[36:42]=[.35,.4];points[42:48]=[.65,.4]
        points[30]=[.5,.55];points[48]=[.4,.7];points[54]=[.6,.7]
        box=np.array([50,30,150,150],np.float32)
        matrix=crop_matrix(box)
        world=(points*256-matrix[:,2])/matrix[0,0]
        return points,box,world,five_points(world)

    def test_crop_and_68_to_5_conversion_round_trip(self):
        raw,box,world,detected=self.fixture()
        matrix=crop_matrix(box)
        np.testing.assert_allclose(matrix@np.array([100,90,1]),[128,128])
        np.testing.assert_allclose(decode_prediction(raw[None],box,detected),world,atol=1e-5)
        np.testing.assert_allclose(five_points(world)[0],world[36:42].mean(0))
        np.testing.assert_allclose(five_points(world)[1],world[42:48].mean(0))
        np.testing.assert_allclose(five_points(world)[2:],world[[30,48,54]])

    def test_bad_predictions_fall_back_without_leaking_prior_landmarks(self):
        raw,box,world,detected=self.fixture()
        for bad in (np.zeros_like(raw),np.full_like(raw,np.nan),raw+2,np.zeros((67,2))):
            with self.subTest(shape=bad.shape):self.assertIsNone(decode_prediction(bad,box,detected))
        for bad in ([0,0,0,0],[2,1,0,4],[0,0,np.inf,3]):
            with self.assertRaises(ValueError):crop_matrix(bad)

    def test_static_graph_keeps_original_and_prunes_auxiliary_output(self):
        import onnx
        from onnx import helper,TensorProto
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'hrffa.onnx'
            inputs=[helper.make_tensor_value_info('input',TensorProto.FLOAT,['batch',3,256,256])]
            outputs=[helper.make_tensor_value_info('points',TensorProto.FLOAT,['batch',68,2]),
                     helper.make_tensor_value_info('logits',TensorProto.FLOAT,['batch',68,3])]
            nodes=[helper.make_node('Identity',['input'],['points']),helper.make_node('Identity',['input'],['logits'])]
            onnx.save(helper.make_model(helper.make_graph(nodes,'fixture',inputs,outputs)),str(source))
            original=source.read_bytes()
            target=static_landmarker(source,root/'cuda-kernels')
            model=onnx.load(str(target))
            self.assertEqual(len(model.graph.output),1)
            self.assertEqual([d.dim_value for d in model.graph.output[0].type.tensor_type.shape.dim],[1,68,2])
            self.assertEqual(source.read_bytes(),original)
            self.assertEqual(static_landmarker(source,root/'cuda-kernels'),target)


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA test')
class LandmarkerGpuTests(unittest.TestCase):
    def test_refine_and_no_face_clear_use_only_small_coordinate_readback(self):
        import cupy as cp
        from types import SimpleNamespace
        from face_swap_landmarker import GpuLandmarker
        raw,box,world,detected=LandmarkerTests().fixture()
        landmarker=GpuLandmarker.__new__(GpuLandmarker)
        landmarker.stream=cp.cuda.Stream(non_blocking=True)
        landmarker.host=np.empty((1,68,2),np.float32)
        landmarker.points=None;landmarker.inverse=None
        calls=[]
        landmarker.pack=lambda *args:calls.append(args)
        landmarker.model=SimpleNamespace(inputs=[None],outputs=[cp.asarray(raw[None])],run=lambda:None)
        with landmarker.stream:
            actual=landmarker.refine(None,200,180,box,detected)
            np.testing.assert_allclose(actual,detected,atol=1e-5)
            self.assertIsNotNone(landmarker.points)
            landmarker.model.outputs[0].fill(np.nan)
            np.testing.assert_array_equal(landmarker.refine(None,200,180,box,detected),detected)
            self.assertIsNone(landmarker.points)
            self.assertIsNone(landmarker.inverse)
            landmarker.clear();self.assertIsNone(landmarker.points)
        self.assertEqual(len(calls),2)


if __name__=='__main__':unittest.main()
