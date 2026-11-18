import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

from face_swap_occlusion import KERNELS, static_model


@unittest.skipUnless(importlib.util.find_spec("onnx"), "ONNX required")
class StaticOccluderTests(unittest.TestCase):
    def test_batch_is_frozen_in_separate_cache_without_changing_original(self):
        import onnx
        from onnx import helper, TensorProto
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source=root/'original.onnx'
            graph=helper.make_graph([helper.make_node('Identity',['input'],['output'])],'fixture',
                [helper.make_tensor_value_info('input',TensorProto.FLOAT,['batch',256,256,3])],
                [helper.make_tensor_value_info('output',TensorProto.FLOAT,['batch',256,256,3])])
            onnx.save(helper.make_model(graph),source)
            before=hashlib.sha256(source.read_bytes()).hexdigest()
            prepared=static_model(source,root/'cuda-kernels')
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
            self.assertEqual(prepared.parent,root/'prepared')
            model=onnx.load(prepared)
            for tensor in list(model.graph.input)+list(model.graph.output):
                self.assertEqual(tensor.type.tensor_type.shape.dim[0].dim_value,1)
            self.assertEqual(static_model(source,root/'cuda-kernels'),prepared)


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1', 'Opt-in CUDA test')
class OcclusionGpuTests(unittest.TestCase):
    def test_bgr_nhwc_crop_and_alpha_resizing_for_swap_and_restoration(self):
        import numpy as np
        import cv2
        import cupy as cp
        module=cp.RawModule(code=KERNELS)
        pack=module.get_function('pack_occlusion')
        combine=module.get_function('combine_occlusion')
        y,x=np.mgrid[:256,:256]
        image=np.stack([x,y,np.full_like(x,31),np.full_like(x,255)],axis=-1).astype(np.uint8)
        gpu_image=cp.asarray(image)
        tensor=cp.empty((1,256,256,3),dtype=cp.float32)
        pack((256,), (256,), (gpu_image,np.int32(256),np.int32(256),tensor,
            *(np.float32(v) for v in (1,0,0,0,1,0))))
        np.testing.assert_allclose(cp.asnumpy(tensor)[0],image[:,:,:3]/255,atol=1e-7)
        for side in (128,256,512):
            ellipse=cp.full((side,side),.7,dtype=cp.float32)
            output=cp.empty_like(ellipse)
            for data in (np.zeros((256,256),np.float32),np.ones((256,256),np.float32),x.astype(np.float32)/255):
                mask=cp.asarray(data)
                combine(((side*side+255)//256,), (256,), (mask,ellipse,output,np.int32(side)))
                reference=np.clip((cv2.resize(data,(side,side),interpolation=cv2.INTER_LINEAR)-.5)*2,0,1)*.7
                np.testing.assert_allclose(cp.asnumpy(output),reference,atol=2e-6)
                np.testing.assert_array_equal(cp.asnumpy(ellipse),np.full((side,side),.7,np.float32))


if __name__=='__main__':unittest.main()
