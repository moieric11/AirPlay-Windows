import os
import unittest
import numpy as np
from face_swap_shape import ShapeGeometry,desired_shape,triangle_table,pose_weight,ANCHORS,INDICES


def face_points():
    p=np.zeros((68,2),np.float64)
    angle=np.linspace(np.pi,0,17)
    p[:17]=np.stack([.5+.30*np.cos(angle),.52+.36*np.sin(angle)],axis=1)
    p[17:22]=np.stack([np.linspace(.25,.45,5),[.36,.34,.33,.34,.36]],axis=1)
    p[22:27]=np.stack([np.linspace(.55,.75,5),[.36,.34,.33,.34,.36]],axis=1)
    p[27:31]=np.stack([np.full(4,.5),np.linspace(.4,.60,4)],axis=1)
    p[31:36]=[[.43,.62],[.46,.64],[.5,.65],[.54,.64],[.57,.62]]
    for start,x in ((36,.35),(42,.65)):
        p[start:start+6]=np.array([[-.08,0],[-.04,-.025],[.04,-.025],[.08,0],[.04,.025],[-.04,.025]])+[x,.46]
    angle=np.linspace(np.pi,-np.pi,12,endpoint=False)
    p[48:60]=np.stack([.5+.14*np.cos(angle),.75+.05*np.sin(angle)],axis=1)
    angle=np.linspace(np.pi,-np.pi,8,endpoint=False)
    p[60:68]=np.stack([.5+.09*np.cos(angle),.75+.02*np.sin(angle)],axis=1)
    return p


class ShapeTests(unittest.TestCase):
    def test_zero_and_same_geometry_are_noops_and_reset_recalibrates(self):
        current=face_points();source=current.copy();source[:17,0]=.5+(source[:17,0]-.5)*.8
        geometry=ShapeGeometry(source)
        self.assertIsNone(geometry.update(current,0))
        self.assertIsNotNone(geometry.update(current,60))
        geometry.reset();self.assertIsNone(geometry.reference)
        self.assertIsNone(ShapeGeometry(current).update(current,100))

    def test_neutral_offsets_preserve_live_eye_opening_and_mouth_motion(self):
        ref=face_points();source=ref.copy();source[36:42,0]=.35+(source[36:42,0]-.35)*1.15
        source[48:60,1]+=.03
        current=ref.copy();current[37:39,1]-=.02;current[40:42,1]+=.02
        current[48:68,1]+=.04
        desired=desired_shape(current,source,ref,.75)
        np.testing.assert_allclose(desired[36:42,1],current[36:42,1])
        np.testing.assert_allclose(desired[[48,54],1]-current[[48,54],1],.03*.75)
        np.testing.assert_array_equal(desired[60:68],current[60:68])

    def test_inverse_mesh_maps_donor_shaped_boundary_back_to_original_face(self):
        ref=face_points();source=ref.copy();source[:17,0]=.5+(source[:17,0]-.5)*.8
        geometry=ShapeGeometry(source);table=geometry.update(ref,100)
        desired=desired_shape(ref,source,ref,geometry.weight)
        index=4;point=desired[index]
        mapped=[]
        for row in table:
            u=row[4:7]@[*point,1];v=row[7:10]@[*point,1]
            if u>=-1e-5 and v>=-1e-5 and u+v<=1+1e-5:
                mapped.append([row[10:13]@[*point,1],row[13:16]@[*point,1]])
        self.assertTrue(mapped)
        np.testing.assert_allclose(mapped, np.tile(ref[index],(len(mapped),1)),atol=1e-5)
        np.testing.assert_array_equal(ANCHORS,np.concatenate([desired[INDICES],ANCHORS])[-8:])

    def test_profile_and_invalid_landmarks_do_not_capture_bad_calibration(self):
        ref=face_points();geometry=ShapeGeometry(ref)
        self.assertIsNone(geometry.update(ref,80,pose=.1));self.assertIsNone(geometry.reference)
        profile=ref.copy();profile[30,0]+=.2
        self.assertEqual(pose_weight(profile),0)
        self.assertIsNone(geometry.update(np.full((68,2),np.nan),80))


@unittest.skipUnless(os.environ.get('FACE_SWAP_GPU_TESTS')=='1','Opt-in CUDA test')
class ShapeGpuTests(unittest.TestCase):
    def test_donor_shape_guides_bound_input_before_inference_without_editing_video(self):
        import cupy as cp
        from face_swap_shape import GpuShapeTransfer
        ref=face_points();source=ref.copy();source[:17,0]=.5+(source[:17,0]-.5)*.8
        shape=GpuShapeTransfer(cp,source*128)
        shape.geometry=ShapeGeometry(source);shape.alignment=1;shape.generation=0
        y,x=np.mgrid[:128,:128]
        pixels=np.broadcast_to(np.array([25,80,160],np.float32)[:,None,None],(3,128,128)).copy()
        inside=((x/128-.5)/.30)**2+((y/128-.52)/.36)**2<1
        pixels[:,inside]=np.array([210,150,90],np.float32)[:,None]
        tensor=cp.asarray(pixels[None]);pointer=tensor.data.ptr
        video=cp.full((256,256,4),73,cp.uint8)
        matrix=np.array([[1,0,0],[0,1,0]],np.float64)
        self.assertFalse(shape.prepare(ref*128,matrix,128,0,1,0))
        self.assertFalse(shape.guide_input(tensor))
        np.testing.assert_array_equal(cp.asnumpy(tensor)[0],pixels)
        self.assertTrue(shape.prepare(ref*128,matrix,128,75,1,0))
        self.assertTrue(shape.guide_input(tensor))
        result=cp.asnumpy(tensor)[0]
        self.assertGreater(np.count_nonzero(result!=pixels),100)
        self.assertEqual(tensor.data.ptr,pointer,'ORT input binding was replaced')
        np.testing.assert_array_equal(cp.asnumpy(video),np.full((256,256,4),73,np.uint8))
        mask=cp.asarray(inside.astype(np.float32));original=cp.asnumpy(mask)
        coverage=cp.asnumpy(shape.coverage(mask,128))
        self.assertTrue((coverage>=original).all(),'Original silhouette can leak when generated face narrows')
        np.testing.assert_array_equal(cp.asnumpy(mask),original)
        shape.prepare(None,None,128,75,1,0)
        self.assertIsNone(shape.geometry.reference)

    def test_occlusion_suppression_keeps_input_exact_and_protected_features_stay_anchored(self):
        import cupy as cp
        from types import SimpleNamespace
        from face_swap_shape import GpuShapeTransfer
        ref=face_points();source=ref.copy();source[:17,0]=.5+(source[:17,0]-.5)*.8
        shape=GpuShapeTransfer(cp,source*128)
        shape.geometry=ShapeGeometry(source);shape.alignment=1;shape.generation=0
        matrix=np.array([[1,0,0],[0,1,0]],np.float64)
        original=cp.arange(3*128*128,dtype=cp.float32).reshape(1,3,128,128)
        tensor=original.copy()
        self.assertTrue(shape.prepare(ref*128,matrix,128,80,1,0,SimpleNamespace(blurred=cp.zeros((256,256),cp.float32))))
        shape.guide_input(tensor)
        np.testing.assert_array_equal(cp.asnumpy(tensor),cp.asnumpy(original))
        source[36:48]+=.04;source[[48,54]]+=.03
        result=desired_shape(ref,source,ref,1,preserve_mode=4)
        np.testing.assert_array_equal(result[36:48],ref[36:48])
        np.testing.assert_array_equal(result[[48,54]],ref[[48,54]])


if __name__=='__main__':unittest.main()
