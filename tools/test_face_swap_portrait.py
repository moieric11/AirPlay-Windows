import unittest
import os
import numpy as np
from unittest.mock import Mock, patch
from face_swap_portrait import driven_points, transformed_points, rotation, landmark_pose, POSE_MODEL, POSE_INDICES


def motion(seed):
    rng=np.random.default_rng(seed)
    return dict(pitch=np.float32(5),yaw=np.float32(-7),roll=np.float32(2),
                scale=np.ones((1,1),np.float32),translation=np.zeros((1,3),np.float32),
                expression=rng.normal(0,.01,(1,21,3)).astype(np.float32),
                motion_points=rng.normal(0,.1,(1,21,3)).astype(np.float32))


class PortraitTests(unittest.TestCase):
    def test_registration_recovers_known_position_scale_and_roll(self):
        import cv2
        from face_swap_portrait import registration_matrix
        source=np.array([[180,210],[310,211],[245,285],[202,355],[294,353]],np.float64)
        expected=cv2.getRotationMatrix2D((256,256),8,1.08);expected[:,2]+=[9,-12]
        target=cv2.transform(source[None],expected)[0]
        matrix=registration_matrix(source,target,512)
        np.testing.assert_allclose(matrix,expected,atol=2e-5)

    def test_registration_rejects_unreliable_fits(self):
        from face_swap_portrait import registration_matrix
        source=np.array([[180,210],[310,211],[245,285],[202,355],[294,353]],np.float64)
        for target in (source*3,source+300,np.full((5,2),np.nan)):
            self.assertIsNone(registration_matrix(source,target,512))
        self.assertIsNone(registration_matrix(np.zeros((5,2)),source,512))

    def test_large_centred_rotation_is_not_rejected_by_origin_translation(self):
        import cv2
        from face_swap_portrait import registration_matrix
        source=np.array([[180,210],[310,211],[245,285],[202,355],[294,353]],np.float64)
        expected=cv2.getRotationMatrix2D(tuple(source.mean(0)),45,1.4)
        self.assertGreater(float(np.max(np.abs(expected[:,2]))),512*.3)
        target=cv2.transform(source[None],expected)[0]
        np.testing.assert_allclose(registration_matrix(source,target,512),expected,atol=2e-5)

    def test_bounded_correction_still_tracks_target_centre(self):
        import cv2
        from face_swap_portrait import registration_matrix
        source=np.array([[180,210],[310,211],[245,285],[202,355],[294,353]],np.float64)
        desired=cv2.getRotationMatrix2D(tuple(source.mean(0)),65,2.2);desired[:,2]+=[15,-20]
        target=cv2.transform(source[None],desired)[0]
        fitted=registration_matrix(source,target,512)
        np.testing.assert_allclose(cv2.transform(source[None],fitted)[0].mean(0),target.mean(0),atol=3e-5)
        self.assertAlmostEqual(float(np.linalg.norm(fitted[:,0])),1.8,places=5)

    def test_generated_landmark_measurement_preserves_target_overlay(self):
        from types import SimpleNamespace
        from face_swap_portrait import PortraitRegistration
        registration=PortraitRegistration.__new__(PortraitRegistration)
        registration.cp=np;registration.side=512
        registration.source_points=np.zeros((5,2),np.float32);registration.source_bbox=np.array([0,0,512,512])
        registration.image=np.zeros((512,512,4),np.uint8);registration.convert=lambda *args:None
        raw=np.arange(136,dtype=np.float32).reshape(1,68,2)
        saved=np.ones((68,2),np.float32);inverse=np.eye(2,3)
        marker=SimpleNamespace(points=saved,inverse=inverse,model=SimpleNamespace(outputs=[raw.copy()]),metadata_bytes=544)
        def rejected_measurement(*args,**kwargs):
            marker.points=None;marker.inverse=None;marker.model.outputs[0][:]=-1
        marker.refine=rejected_measurement;registration.landmarker=marker;registration.raw_backup=np.empty_like(raw)
        pixels=np.zeros((1,3,512,512),np.float32)
        self.assertIsNone(registration.align(pixels,saved,np.eye(2,3)))
        self.assertIs(marker.points,saved);self.assertIs(marker.inverse,inverse)
        np.testing.assert_array_equal(marker.model.outputs[0],raw)
        self.assertEqual(registration.metadata_bytes,544)

    @unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA test')
    def test_gpu_registration_warp_uses_inverse_and_preserves_rgb(self):
        import cupy as cp
        from face_swap_portrait import REGISTRATION_KERNELS
        side=32;y,x=np.mgrid[:side,:side].astype(np.float32)
        image=np.stack((x/side,y/side,np.full_like(x,.5)))[None]
        source=cp.asarray(image);output=cp.empty_like(source)
        kernel=cp.RawModule(code=REGISTRATION_KERNELS).get_function('register_portrait')
        kernel((4,),(256,),(source,output,np.int32(side),
               np.float32(1),np.float32(0),np.float32(-3),np.float32(0),np.float32(1),np.float32(-5)))
        expected=image[:,:,np.clip(np.arange(side)-5,0,side-1)[:,None],np.clip(np.arange(side)-3,0,side-1)]
        np.testing.assert_allclose(cp.asnumpy(output),expected,atol=1e-7)

    def test_rotation_is_rigid(self):
        r=rotation(np,20,-30,15)
        np.testing.assert_allclose(r@r.T,np.eye(3),atol=2e-7)
        self.assertAlmostEqual(float(np.linalg.det(r)),1,places=6)

    def test_direct_driver_expression_preserves_donor_geometry(self):
        source,driver=motion(1),motion(2)
        np.testing.assert_allclose(driven_points(np,source,driver,100),
                                   source['motion_points']@rotation(np,driver['pitch'],driver['yaw'],driver['roll'])+driver['expression'],atol=1e-7)

    def test_zero_strength_freezes_reference(self):
        source,neutral,driver=motion(1),motion(2),motion(3)
        driver['yaw']=np.float32(30);driver['scale']*=1.5;driver['translation']+=.05
        np.testing.assert_allclose(driven_points(np,source,driver,0),
                                   transformed_points(np,source),atol=1e-7)

    def test_driver_canonical_geometry_is_ignored(self):
        source,neutral,driver=motion(1),motion(2),motion(3)
        before=driven_points(np,source,driver,100)
        driver['motion_points'][:]=999;neutral['motion_points'][:]=-999
        np.testing.assert_array_equal(before,driven_points(np,source,driver,100))

    def test_residual_position_is_not_frozen_at_donor_center(self):
        source,neutral=motion(1),motion(2)
        driver={key:value.copy() for key,value in neutral.items()}
        driver['expression']=source['expression'].copy()
        driver['translation'][...,0]+=.03
        difference=driven_points(np,source,driver,100)-transformed_points(np,source)
        np.testing.assert_allclose(difference[...,0],.03,atol=1e-7)

    def test_absolute_pose_does_not_add_donor_orientation(self):
        source,neutral,driver=motion(1),motion(2),motion(3)
        source['yaw']=np.float32(20);neutral['yaw']=np.float32(-10);driver['yaw']=np.float32(15)
        driver['expression']=source['expression'].copy()
        actual=driven_points(np,source,driver,100)
        expected=source['motion_points']@rotation(np,driver['pitch'],driver['yaw'],driver['roll'])+source['expression']
        np.testing.assert_allclose(actual,expected,atol=1e-7)

    def test_hrffa_pose_replaces_neural_pose(self):
        source,neutral,driver=motion(1),motion(2),motion(3)
        driver['expression']=source['expression'].copy()
        actual=driven_points(np,source,driver,100,np.array([5,25,2],np.float32))
        driver['yaw']=np.float32(-50)
        np.testing.assert_array_equal(actual,driven_points(np,source,driver,100,np.array([5,25,2],np.float32)))
        expected=source['motion_points']@rotation(np,5,25,2)+source['expression']
        np.testing.assert_allclose(actual,expected,atol=1e-7)

    def test_hrffa_pose_recovers_controlled_3d_movements(self):
        import cv2
        camera=np.array([[256,0,128],[0,256,128],[0,0,1]],np.float64)
        for angles in ((0,0,0),(12,25,7),(-10,-25,-5)):
            with self.subTest(angles=angles):
                rvec,_=cv2.Rodrigues(rotation(np,*angles).T.astype(np.float64))
                image,_=cv2.projectPoints(POSE_MODEL,rvec,np.array([0,0,1400.],np.float64),camera,None)
                points=np.zeros((68,2));points[POSE_INDICES]=image.reshape(-1,2)
                np.testing.assert_allclose(landmark_pose(points,np.array([[1,0,0],[0,1,0]])),angles,atol=1e-4)

    def test_invalid_hrffa_pose_is_rejected(self):
        matrix=np.array([[1,0,0],[0,1,0]])
        self.assertIsNone(landmark_pose(np.zeros((5,2)),matrix))
        self.assertIsNone(landmark_pose(np.full((68,2),np.nan),matrix))

    def test_expression_follows_current_frame_without_neutral_delta(self):
        source,driver=motion(1),motion(2)
        actual=driven_points(np,source,driver,100)
        expected=source['motion_points']@rotation(np,driver['pitch'],driver['yaw'],driver['roll'])+driver['expression']
        np.testing.assert_allclose(actual,expected,atol=1e-7)
        half=driven_points(np,source,driver,50)
        expected=source['motion_points']@rotation(np,driver['pitch'],driver['yaw'],driver['roll'])+(source['expression']+driver['expression'])*.5
        np.testing.assert_allclose(half,expected,atol=1e-7)

    def test_animator_output_does_not_depend_on_previous_frames(self):
        from face_swap_portrait import PortraitAnimator,FIELDS
        from types import SimpleNamespace
        animator=PortraitAnimator.__new__(PortraitAnimator)
        animator.cp=np;animator.source=motion(1);animator.motion_names=list(FIELDS)
        current=motion(2)
        animator.motion=SimpleNamespace(outputs=[current[key] for key in FIELDS],run=lambda:None)
        animator.stitch_inputs={'source':np.empty((1,21,3),np.float32),'target':np.empty((1,21,3),np.float32)}
        animator.source_points=transformed_points(np,animator.source)
        animator.stitch=SimpleNamespace(outputs=[animator.stitch_inputs['source']],run=lambda:None)
        animator.generator_inputs={'source':np.empty((1,21,3),np.float32)}
        animator.generator=SimpleNamespace(outputs=[animator.generator_inputs['source']],run=lambda:None)
        first=animator.animate(100).copy()
        other=motion(3);other['yaw']=np.float32(30)
        animator.motion.outputs=[other[key] for key in FIELDS];animator.animate(100)
        animator.motion.outputs=[current[key] for key in FIELDS]
        np.testing.assert_array_equal(first,animator.animate(100))
        for key in ('reference','hrffa_reference','generation'):self.assertFalse(hasattr(animator,key))

    def test_selected_builder_level_and_cuda_fallback(self):
        from face_swap_portrait import make_portrait_model
        for level in (3,5):
            with self.subTest(level=level), patch('face_swap_tensorrt.TensorRTModel') as trt:
                model,backend=make_portrait_model(None,'portrait.onnx',None,0,'swappers/cuda-kernels',level)
                self.assertIs(model,trt.return_value)
                self.assertEqual(backend,'TensorRT FP32')
                self.assertEqual(trt.call_args.kwargs['builder_level'],level)
                model.run.assert_called_once()
        with patch('face_swap_tensorrt.TensorRTModel',side_effect=RuntimeError('build failed')) as trt, \
             patch('face_swap_gpu_worker.DeviceModel',return_value=Mock(graph_ready=True)) as cuda:
            model,backend=make_portrait_model(None,'portrait.onnx',None,0,'swappers/cuda-kernels',5)
            trt.assert_called_once()
            self.assertEqual(trt.call_args.kwargs['builder_level'],5)
            self.assertIs(model,cuda.return_value)
            self.assertEqual(backend,'CUDA Graph')

    def test_live_strength_bounds(self):
        from face_swap_mask import normalize_settings
        self.assertEqual(normalize_settings({})['animation_strength'],100)
        for value in (-1,101,1.5,True):
            with self.assertRaises(ValueError):normalize_settings({'animation_strength':value})


if __name__=='__main__':unittest.main()
