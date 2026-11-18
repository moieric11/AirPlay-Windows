import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

from face_swap_parser import KERNELS, region_bits, static_parser


class RegionSelectionTests(unittest.TestCase):
    def test_modes_preserve_eyes_and_mouth_without_excluding_skin(self):
        for mode in (1,2,3,4):
            bits=region_bits(mode)
            self.assertTrue(bits & (1<<1))
            self.assertFalse(bits & 1)
            self.assertEqual(bool(bits & (1<<4)),mode not in (2,4))
            self.assertEqual(bool(bits & (1<<6)),mode not in (2,4))
            self.assertEqual(bool(bits & (1<<12)),mode not in (3,4))
        for mode in (0,5):
            with self.assertRaises(ValueError):region_bits(mode)

    @unittest.skipUnless(importlib.util.find_spec('onnxruntime'), 'ONNX Runtime required')
    def test_preparation_preserves_primary_output_and_original_weights(self):
        import numpy as np
        import onnx
        import onnxruntime as ort
        from onnx import helper,TensorProto,numpy_helper
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'parser.onnx'
            weight=numpy_helper.from_array(np.arange(57,dtype=np.float32).reshape(19,3,1,1)/100,'weight')
            graph=helper.make_graph([helper.make_node('Conv',['input','weight'],['output']),
                helper.make_node('Identity',['output'],['aux'])],'fixture',
                [helper.make_tensor_value_info('input',TensorProto.FLOAT,['batch',3,512,512])],
                [helper.make_tensor_value_info(n,TensorProto.FLOAT,['batch',19,512,512]) for n in ('output','aux')],[weight])
            model=helper.make_model(graph,opset_imports=[helper.make_opsetid('',17)]);model.ir_version=10
            onnx.save(model,source);before=source.read_bytes()
            prepared=static_parser(source,root/'cuda-kernels')
            self.assertEqual(source.read_bytes(),before)
            self.assertEqual(len(onnx.load(prepared).graph.output),1)
            a=ort.InferenceSession(str(source),providers=['CPUExecutionProvider'])
            b=ort.InferenceSession(str(prepared),providers=['CPUExecutionProvider'])
            feed={'input':np.ones((1,3,512,512),np.float32)}
            np.testing.assert_array_equal(a.run(None,feed)[0],b.run(None,feed)[0])


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA tests')
class ParserGpuTests(unittest.TestCase):
    def test_rgb_normalization_argmax_and_resized_mask_composition(self):
        import cv2
        import cupy as cp
        import numpy as np
        module=cp.RawModule(code=KERNELS)
        pack=module.get_function('pack_parser');segment=module.get_function('parser_regions')
        combine=module.get_function('combine_parser')
        image=cp.empty((512,512,4),cp.uint8);image[:]=cp.asarray([17,42,199,255],cp.uint8)
        tensor=cp.empty((1,3,512,512),cp.float32)
        pack((1024,),(256,),(image,np.int32(512),np.int32(512),tensor,
            *(np.float32(v) for v in (1,0,0,0,1,0))))
        expected=(np.array([199,42,17],np.float32)/255-np.array([.485,.456,.406],np.float32))/np.array([.229,.224,.225],np.float32)
        np.testing.assert_allclose(cp.asnumpy(tensor)[0,:,0,0],expected,atol=1e-6)
        labels=np.broadcast_to(np.arange(512)%19,(512,512))
        logits=np.stack([(labels==c).astype(np.float32) for c in range(19)])[None]
        gpu_logits=cp.asarray(logits);mask=cp.empty((512,512),cp.float32)
        for mode in (1,2,3,4):
            bits=region_bits(mode)
            segment((1024,),(256,),(gpu_logits,mask,np.uint32(bits)))
            reference=((bits >> labels)&1).astype(np.float32)
            np.testing.assert_array_equal(cp.asnumpy(mask),reference)
        # Composition also receives XSeg's result as its base; never modify it.
        ramp=np.broadcast_to(np.linspace(0,1,512,dtype=np.float32),(512,512)).copy()
        for side in (128,256,512):
            base=cp.full((side,side),.7,cp.float32);out=cp.empty_like(base)
            combine(((side*side+255)//256,),(256,),(cp.asarray(ramp),base,out,np.int32(side)))
            expected=np.clip((cv2.resize(ramp,(side,side))-.5)*2,0,1)*.7
            np.testing.assert_allclose(cp.asnumpy(out),expected,atol=2e-6)
            np.testing.assert_array_equal(cp.asnumpy(base),np.full((side,side),.7,np.float32))


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA test')
class ExpansionProtectionTests(unittest.TestCase):
    def test_expansion_extends_face_but_keeps_eyes_mouth_and_occlusion_interior(self):
        import numpy as np
        import cupy as cp
        from types import SimpleNamespace
        from face_swap_parser import RegionMask
        from face_swap_mask import MaskExpansion
        from face_swap_gpu_worker import GpuImageEngine
        labels=np.zeros((512,512),np.int32)
        labels[128:432,96:416]=1
        labels[192:256,160:224]=4
        labels[304:368,224:288]=11
        logits=cp.asarray(np.stack([(labels==index).astype(np.float32) for index in range(19)])[None])
        parser=RegionMask.__new__(RegionMask)
        parser.regions=region_bits(4);parser.protected_bits=region_bits(1)&~parser.regions
        parser.protection_ready=False;parser.model=SimpleNamespace(outputs=[logits])
        module=cp.RawModule(code=KERNELS)
        parser.segment=module.get_function('parser_regions')
        parser.combine_kernel=module.get_function('combine_parser')
        parser.protect_kernel=module.get_function('protect_parser')
        parser.mask=cp.empty((512,512),cp.float32);parser.blurred=cp.empty_like(parser.mask)
        parser.protected_mask=cp.empty_like(parser.mask);parser.protected_blurred=cp.empty_like(parser.mask)
        parser.combined={side:cp.empty((side,side),cp.float32) for side in (128,256)}
        parser.protected_combined={side:cp.empty_like(value) for side,value in parser.combined.items()}
        parser.filter()
        engine=GpuImageEngine.__new__(GpuImageEngine)
        engine.swap_size=128;engine.restore_size=256;engine.parser=parser
        engine.swap_mask=cp.ones((128,128),cp.float32);engine.restore_mask=cp.ones((256,256),cp.float32)
        engine.mask_expander=MaskExpansion(cp,(128,256))
        visibility={side:cp.ones((side,side),cp.float32) for side in (128,256)}
        for side,value in visibility.items():value[:,int(side*.78):]=0
        combined={side:cp.empty_like(value) for side,value in visibility.items()}
        def occlude(base,side):
            cp.multiply(base,visibility[side],out=combined[side]);return combined[side]
        engine.occluder=SimpleNamespace(combine=occlude)
        for side in (128,256):
            engine.mask_config={'expansion':0};original=cp.asnumpy(engine.blend_mask(side))
            engine.mask_config={'expansion':6};expanded=cp.asnumpy(engine.blend_mask(side))
            self.assertGreater((expanded>.5).sum(),(original>.5).sum())
            self.assertLess(expanded[int(56*side/128),int(48*side/128)],.02)
            self.assertLess(expanded[int(84*side/128),int(64*side/128)],.02)
            radius=max(1,round(side*.06))
            self.assertFalse(expanded[:,int(side*.78)+radius+1:].any())
            np.testing.assert_array_equal(cp.asnumpy(visibility[side])[:,int(side*.78):],0)


if __name__=='__main__':unittest.main()
