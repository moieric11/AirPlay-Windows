import importlib.util
import os
from pathlib import Path
import unittest
import numpy as np
from face_swap_detectors import decode, nms, resize_shape, rescale_rows


def fixture(kind):
    if kind=='yoloface':
        raw=np.zeros((1,20,8400),np.float32)
        for i in (0,6400,8399):
            raw[0,:5,i]=[100+i%50,120,40,60,.8]
            raw[0,5:,i]=np.arange(15,dtype=np.float32)+30
        return [raw]
    side=320 if kind=='retinaface' else 640
    counts=[(side//s)**2*(2 if kind=='retinaface' else 1) for s in (8,16,32)]
    groups=(1,4,10) if kind=='retinaface' else (1,1,4,10)
    outputs=[np.zeros((n,c),np.float32) for c in groups for n in counts]
    for head,n in enumerate(counts):
        outputs[head][n//2,0]=.8
        if kind=='yunet':outputs[head+3][n//2,0]=.9
        group=3 if kind=='retinaface' else 6
        outputs[head+group][n//2]=[.5,.7,1.2,1.4]
        outputs[head+group+3][n//2]=np.arange(10,dtype=np.float32)/10
    return outputs


class DetectorCpuTests(unittest.TestCase):
    def test_decode_shapes_thresholds_and_duplicate_suppression(self):
        for kind in ('yoloface','retinaface','yunet'):
            rows=decode(fixture(kind),kind,320 if kind=='retinaface' else 640)
            self.assertEqual(rows.shape,(3,15))
            self.assertTrue(np.isfinite(rows).all())
            self.assertTrue((rows[:,2]>rows[:,0]).all())
        rows=np.array([[0,0,10,10,.9],[1,1,11,11,.8],[100,100,120,120,.7]],np.float32)
        self.assertEqual(nms(rows),[0,2])
        self.assertEqual(resize_shape(1920,1080,640),(640,360))
        self.assertEqual(resize_shape(1080,1920,320),(180,320))
        rows=np.ones((1,15),np.float32)
        rescale_rows(rows,100,201,318,640)
        self.assertAlmostEqual(rows[0,0],100/318)
        self.assertAlmostEqual(rows[0,1],201/640)
        self.assertEqual(rows[0,4],1)

    @unittest.skipUnless(importlib.util.find_spec('onnxruntime'),'ONNX Runtime required')
    def test_local_official_models_detect_face_on_cpu(self):
        import cv2
        from face_swap_detectors import SourceDetector
        root=Path('build/Release/face_swap/swappers')
        photo=cv2.imread('build/swapper-models/source-fixture.png')
        if photo is None:self.skipTest('Local face fixture not installed')
        for kind,name in (('yoloface','yoloface_8n'),('retinaface','retinaface_10g'),('yunet','yunet_2023_mar')):
            path=root/(name+'.onnx')
            if not path.exists():self.skipTest('Local optional model not installed')
            detector=SourceDetector(path,kind,['CPUExecutionProvider'],None,root/'prepared')
            boxes,points=detector.detect(photo)
            self.assertGreater(len(boxes),0,kind)
            self.assertEqual(points.shape,(len(boxes),5,2))
            self.assertTrue(np.isfinite(points).all())


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA tests')
class DetectorGpuTests(unittest.TestCase):
    def test_gpu_decode_matches_cpu_and_clears_previous_candidates(self):
        import cupy as cp
        from face_swap_detectors import Metadata
        stream=cp.cuda.Stream(non_blocking=True)
        for kind in ('yoloface','yunet'):
            values=fixture(kind)
            with stream:
                outputs=[cp.asarray(v) for v in values]
                metadata=Metadata(cp,outputs,kind)
                actual=metadata.download(stream)
                expected=decode(values,kind,640)
                np.testing.assert_allclose(actual,expected,rtol=1e-6,atol=1e-4)
                for output in outputs:output.fill(0)
                self.assertEqual(len(metadata.download(stream)),0)


if __name__=='__main__':unittest.main()
