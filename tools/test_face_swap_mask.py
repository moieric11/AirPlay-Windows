import os
import unittest

import numpy as np
from face_swap_mask import geometric_mask, MaskPreview, MaskExpansion, normalize_settings, resize_face_placement


class GeometricMaskTests(unittest.TestCase):
    def test_default_is_pixel_exact_legacy_ellipse(self):
        for side in (128,256,512):
            y,x=np.mgrid[:side,:side].astype(np.float32)
            radius=np.sqrt(((x-(side-1)/2)/(side*.46))**2+((y-(side-1)/2)/(side*.49))**2)
            alpha=np.clip((1-radius)/.18,0,1)
            np.testing.assert_array_equal(geometric_mask(side),alpha*alpha*(3-2*alpha))

    def test_independent_padding_edges_and_bounded_softness(self):
        for shape in ('ellipse','box'):
            original=geometric_mask(256,shape,blur=0)
            for index in range(4):
                padding=[0,0,0,0];padding[index]=20
                mask=geometric_mask(256,shape,padding,0)
                self.assertLess(mask.sum(),original.sum())
                if index==0:self.assertFalse(mask[:51,:].any())
                if index==1:self.assertFalse(mask[:,-51:].any())
                if index==2:self.assertFalse(mask[-51:,:].any())
                if index==3:self.assertFalse(mask[:,:51].any())
            for blur in (0,18,50):
                mask=geometric_mask(128,shape,(45,45,45,45),blur)
                self.assertTrue(np.isfinite(mask).all())
                self.assertGreater(mask.max(),0)
                self.assertGreaterEqual(mask.min(),0)
                self.assertLessEqual(mask.max(),1)
        self.assertTrue((geometric_mask(128,'box',blur=0)==1).all())

    def test_face_scale_bounds_and_legacy_default(self):
        self.assertEqual(normalize_settings({})['face_scale'],100)
        for value in (80,100,105,120):
            self.assertEqual(normalize_settings({'face_scale':value})['face_scale'],value)
        for value in (79,121,True,105.5):
            with self.assertRaises(ValueError):normalize_settings({'face_scale':value})

    def test_global_size_rotated_placement_preserves_centre_and_scales_extent(self):
        angle=.4
        matrix=np.array([[np.cos(angle),-np.sin(angle),-25],
                         [np.sin(angle),np.cos(angle),-31]],np.float64)
        inverse=np.linalg.inv(np.vstack([matrix,[0,0,1]]))
        centre=inverse@np.array([31.5,31.5,1])
        self.assertIs(resize_face_placement(matrix,64,100),matrix)
        for size in (80,105,120):
            resized=resize_face_placement(matrix,64,size)
            np.testing.assert_allclose(resized@centre,[31.5,31.5],atol=1e-12)
            enlarged=np.linalg.inv(np.vstack([resized,[0,0,1]]))
            corner=np.array([0,0,1])
            np.testing.assert_allclose(enlarged@corner-centre,(inverse@corner-centre)*size/100,atol=1e-12)
        self.assertEqual(normalize_settings({})['face_size'],100)
        for value in (79,121,True,105.5):
            with self.assertRaises(ValueError):normalize_settings({'face_size':value})
        np.testing.assert_array_equal(matrix[:2,2],[-25,-31])

    def test_expansion_defaults_and_range(self):
        self.assertEqual(normalize_settings({})['expansion'],0)
        for value in (-1,13,True,1.5):
            with self.assertRaises(ValueError):normalize_settings({'expansion':value})

    def test_shape_transfer_defaults_and_range(self):
        settings=normalize_settings({})
        self.assertEqual(settings['shape_transfer'],0);self.assertEqual(settings['shape_reset'],0)
        for bad in ({'shape_transfer':-1},{'shape_transfer':101},{'shape_transfer':True},
                    {'shape_reset':-1},{'shape_reset':2147483648},{'shape_reset':.5}):
            with self.assertRaises(ValueError):normalize_settings(bad)

    def test_invalid_settings_rejected(self):
        for settings in ({'shape':'triangle'},{'padding':(0,0,0)},{'padding':(0,46,0,0)},
                         {'padding':(0,-1,0,0)},{'blur':-1},{'blur':51}):
            with self.subTest(settings=settings),self.assertRaises(ValueError):geometric_mask(256,**settings)


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA test')
class MaskPreviewTests(unittest.TestCase):
    def test_live_settings_keep_model_sessions_and_preview_only_keeps_masks(self):
        import cupy as cp
        from face_swap_gpu_worker import GpuImageEngine
        engine=GpuImageEngine.__new__(GpuImageEngine)
        engine.cp=cp;engine.stream=cp.cuda.Stream(non_blocking=True)
        engine.swap_size=128;engine.restore_size=256
        engine.mask_config=normalize_settings({})
        engine.preview_renderer=MaskPreview(cp,1);engine.mask_preview=None
        sessions=[object() for _ in range(3)]
        engine.detector,engine.swapper,engine.restorer=sessions
        settings={'shape':'box','padding':(8,12,5,10),'blur':25,'preview':1}
        self.assertTrue(engine.configure_mask(settings))
        with engine.stream:engine.stream.synchronize()
        np.testing.assert_array_equal(cp.asnumpy(engine.swap_mask),geometric_mask(128,'box',(8,12,5,10),25))
        np.testing.assert_array_equal(cp.asnumpy(engine.restore_mask),geometric_mask(256,'box',(8,12,5,10),25))
        self.assertEqual([engine.detector,engine.swapper,engine.restorer],sessions)
        previous=engine.swap_mask
        self.assertTrue(engine.configure_mask(dict(settings,shape_transfer=50,shape_reset=1)))
        self.assertIs(engine.swap_mask,previous)
        self.assertEqual([engine.detector,engine.swapper,engine.restorer],sessions)
        self.assertTrue(engine.configure_mask(dict(settings,expansion=4)))
        self.assertIs(engine.swap_mask,previous)
        self.assertEqual([engine.detector,engine.swapper,engine.restorer],sessions)
        self.assertTrue(engine.configure_mask(dict(settings,alignment=1)))
        self.assertIs(engine.swap_mask,previous)
        self.assertEqual([engine.detector,engine.swapper,engine.restorer],sessions)
        self.assertTrue(engine.configure_mask(dict(settings,face_size=110)))
        self.assertIs(engine.swap_mask,previous)
        self.assertTrue(engine.configure_mask(dict(settings,face_scale=105)))
        self.assertIs(engine.swap_mask,previous)
        self.assertEqual([engine.detector,engine.swapper,engine.restorer],sessions)
        self.assertTrue(engine.configure_mask(settings))
        self.assertFalse(engine.configure_mask(settings))
        self.assertTrue(engine.configure_mask(dict(settings,preview=2)))
        self.assertIs(engine.swap_mask,previous)
        self.assertEqual(engine.mask_preview.mode,2)
        self.assertTrue(engine.configure_mask(dict(settings,preview=0)))
        self.assertIsNone(engine.mask_preview)
        with self.assertRaises(ValueError):engine.configure_mask(dict(settings,blur=99))
        self.assertIs(engine.swap_mask,previous)

    def test_face_scale_samples_pixels_without_moving_target_mask(self):
        import cupy as cp
        import cv2
        from types import SimpleNamespace
        from face_swap_gpu_worker import GpuImageEngine, KERNELS
        engine=GpuImageEngine.__new__(GpuImageEngine)
        engine.np=np;engine.base=SimpleNamespace(cv2=cv2);engine.width=engine.height=64
        engine.paste_kernel=cp.RawModule(code=KERNELS).get_function('paste')
        engine.image=cp.full((64,64,4),77,cp.uint8)
        pixels=cp.broadcast_to(cp.arange(64,dtype=cp.float32)[None,None,:],(3,64,64)).copy()
        mask=cp.zeros((64,64),cp.float32);mask[8:56,8:56]=1
        matrix=np.array([[1,0,0],[0,1,0]],np.float32)
        for zoom in (1,1.05,1.2,.8):
            engine.image[:]=77;engine.image[:,:,3]=255
            engine.paste(pixels,mask,64,matrix,1,0,1,zoom)
            actual=cp.asnumpy(engine.image)
            np.testing.assert_array_equal(actual[0,0],[77,77,77,255])
            for x in (16,32,48):
                expected=31.5+(x-31.5)/zoom
                self.assertLessEqual(abs(int(actual[32,x,0])-int(expected)),1)
            self.assertEqual(int(actual[32,7,0]),77)
            self.assertNotEqual(int(actual[32,8,0]),77)
        # Shrinking cannot sample outside the generated crop as black pixels.
        engine.image[:]=77
        engine.paste(pixels,cp.ones((64,64),cp.float32),64,matrix,1,0,1,.8)
        self.assertEqual(int(cp.asnumpy(engine.image)[0,0,0]),77)

    def test_global_face_size_expands_face_and_mask_together(self):
        import cupy as cp
        import cv2
        from types import SimpleNamespace
        from face_swap_gpu_worker import GpuImageEngine, KERNELS
        engine=GpuImageEngine.__new__(GpuImageEngine)
        engine.np=np;engine.base=SimpleNamespace(cv2=cv2);engine.width=engine.height=128
        engine.paste_kernel=cp.RawModule(code=KERNELS).get_function('paste')
        engine.image=cp.full((128,128,4),77,cp.uint8)
        pixels=cp.full((3,64,64),200,cp.float32)
        mask=cp.zeros((64,64),cp.float32);mask[16:48,16:48]=1
        matrix=np.array([[1,0,-32],[0,1,-32]],np.float32)
        results={}
        for size in (80,100,120):
            engine.image[:]=77
            engine.paste(pixels,mask,64,resize_face_placement(matrix,64,size),1,0,1)
            results[size]=cp.asnumpy(engine.image)
            self.assertEqual(int(results[size][64,64,0]),200)
            self.assertEqual(int(results[size][0,0,0]),77)
        self.assertEqual(int(results[100][64,46,0]),77)
        self.assertEqual(int(results[120][64,46,0]),200)
        self.assertEqual(int(results[100][64,49,0]),200)
        self.assertEqual(int(results[80][64,49,0]),77)
        counts=[int((results[size][:,:,0]!=77).sum()) for size in (80,100,120)]
        self.assertLess(counts[0],counts[1]);self.assertLess(counts[1],counts[2])

    def test_facefusion_crop_replicates_borders_like_opencv_without_readback(self):
        import cupy as cp
        import cv2
        from face_swap_gpu_worker import KERNELS
        image=np.arange(4*4*4,dtype=np.uint8).reshape(4,4,4)+20
        tensor=cp.empty((3,8,8),cp.float32)
        pack=cp.RawModule(code=KERNELS).get_function('aligned_pack')
        inverse=np.array([[1,0,-2],[0,1,-2]],np.float32)
        for mode,border in ((0,cv2.BORDER_CONSTANT),(1,cv2.BORDER_REPLICATE)):
            pack((1,),(256,),(cp.asarray(image),np.int32(4),np.int32(4),tensor,np.int32(8),
                *(np.float32(v) for v in inverse.ravel()),np.float32(0),np.float32(1),np.int32(mode)))
            reference=cv2.warpAffine(image,cv2.invertAffineTransform(inverse),(8,8),
                flags=cv2.INTER_LINEAR,borderMode=border)[:,:,:3][:,:,::-1].transpose(2,0,1)
            np.testing.assert_array_equal(cp.asnumpy(tensor),reference)

    def test_expansion_matches_dilation_without_changing_original_mask(self):
        import cv2
        import cupy as cp
        expansion=MaskExpansion(cp,(128,256,512))
        for side in (128,256,512):
            original=np.zeros((side,side),np.float32)
            original[side//4:3*side//4,side//4:3*side//4]=.8
            original[side//3:2*side//3,side//3:2*side//3]=1
            source=cp.asarray(original)
            self.assertIs(expansion.apply(source,side,0),source)
            for amount in (1,4,12):
                radius=max(1,round(side*amount/100))
                expected=cv2.dilate(original,np.ones((2*radius+1,2*radius+1),np.uint8),borderType=cv2.BORDER_CONSTANT,borderValue=0)
                actual=cp.asnumpy(expansion.apply(source,side,amount))
                np.testing.assert_array_equal(actual,expected)
                self.assertGreater((actual>.5).sum(),(original>.5).sum())
                np.testing.assert_array_equal(cp.asnumpy(source),original)

    def test_final_mask_overlay_points_and_no_face_clear(self):
        import cupy as cp
        image=cp.full((64,64,4),100,dtype=cp.uint8);image[:,:,3]=255
        mask=cp.zeros((32,32),cp.float32);mask[8:24,8:24]=1
        matrix=np.array([[1,0,-16],[0,1,-16]],np.float32)
        points=np.array([[20,20],[40,20],[30,30],[24,40],[36,40]],np.float32)
        preview=MaskPreview(cp,2);preview.apply(image,mask,32,matrix,points)
        actual=cp.asnumpy(image)
        np.testing.assert_array_equal(actual[0,0],[0,0,0,255])
        np.testing.assert_array_equal(actual[26,26],[255,255,255,255])
        np.testing.assert_array_equal(actual[20,20],[0,65,255,255])
        # A frame with no face must not retain the previous mask or landmarks.
        preview.apply(image,mask,32,None,None)
        self.assertFalse(cp.asnumpy(image[:,:,:3]).any())
        image[:]=cp.asarray([100,100,100,255],cp.uint8)
        overlay=MaskPreview(cp,1);overlay.apply(image,mask,32,matrix,points)
        actual=cp.asnumpy(image)
        np.testing.assert_array_equal(actual[0,0],[100,100,100,255])
        self.assertGreater(actual[26,26,1],actual[26,26,0])


if __name__=='__main__':unittest.main()
