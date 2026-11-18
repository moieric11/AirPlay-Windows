"""LivePortrait reference-image animation with direct HRFFA pose tracking on CUDA.

Motion equations follow KwaiVGI/LivePortrait (MIT), motion inference.
The pinned ONNX exports combine warping/decoding and expose corrected keypoints.
"""
from pathlib import Path
import numpy as np

NAMES=('live_portrait_feature_extractor','live_portrait_motion_extractor',
       'live_portrait_generator','live_portrait_stitcher')
FIELDS=('pitch','yaw','roll','scale','translation','expression','motion_points')


def portrait_matrix(landmarks,side):
    import cv2
    target=np.array([[.37691676,.46864664],[.62285697,.46912813],
                     [.50123859,.61331904],[.39308822,.72541100],
                     [.61150205,.72490465]],np.float64)*side
    matrix,_=cv2.estimateAffinePartial2D(np.asarray(landmarks,np.float32),target,
                                      method=cv2.RANSAC,ransacReprojThreshold=100)
    if matrix is None or not np.isfinite(matrix).all():raise ValueError('Cannot align portrait landmarks')
    return matrix


def registration_matrix(generated,target,side):
    """Fit a proper similarity in crop pixels; preserve the donor's proportions."""
    source=np.asarray(generated,np.float64);target=np.asarray(target,np.float64)
    if source.shape!=(5,2) or target.shape!=(5,2) or not np.isfinite(source).all() or not np.isfinite(target).all():return None
    x=source-source.mean(0);y=target-target.mean(0)
    variance=float((x*x).sum())
    if variance<1:return None
    u,s,vt=np.linalg.svd(x.T@y)
    sign=np.array([1.,np.linalg.det(u@vt)])
    row=(u*sign)@vt;scale=float(s@sign/variance)
    angle=np.rad2deg(np.arctan2(row[0,1],row[0,0]))
    # Translation about image origin can be large for a perfectly centred fit.
    # Validate centroid movement instead; bound scale/roll while retaining the
    # current target centroid even when the correction reaches a soft limit.
    if not .35<=scale<=2.5 or abs(angle)>75 or np.linalg.norm(target.mean(0)-source.mean(0))>side*.55:return None
    scale=float(np.clip(scale,.55,1.8));angle=np.deg2rad(np.clip(angle,-55,55))
    linear=scale*np.array([[np.cos(angle),-np.sin(angle)],[np.sin(angle),np.cos(angle)]])
    matrix=np.column_stack((linear,target.mean(0)-linear@source.mean(0)))
    return matrix.astype(np.float32)


REGISTRATION_KERNELS=r'''
extern "C" __global__ void portrait_bgra(const float* rgb,unsigned char* bgra,int n) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=n*n)return;
 for(int ch=0;ch<3;++ch)bgra[k*4+ch]=(unsigned char)(__saturatef(rgb[(2-ch)*n*n+k])*255.f+.5f);
 bgra[k*4+3]=255;
}
extern "C" __global__ void register_portrait(const float* src,float* dst,int n,
 float a,float b,float c,float d,float e,float f) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=n*n)return;
 float x=fminf(n-1.f,fmaxf(0.f,a*(k%n)+b*(k/n)+c));
 float y=fminf(n-1.f,fmaxf(0.f,d*(k%n)+e*(k/n)+f));
 int ix=(int)x,iy=(int)y,jx=min(ix+1,n-1),jy=min(iy+1,n-1);
 float dx=x-ix,dy=y-iy;
 for(int ch=0;ch<3;++ch) {
  const float* p=src+ch*n*n;
  dst[ch*n*n+k]=(1-dy)*((1-dx)*p[iy*n+ix]+dx*p[iy*n+jx])+dy*((1-dx)*p[jy*n+ix]+dx*p[jy*n+jx]);
 }
}
'''


class PortraitRegistration:
    """Measure generated HRFFA points and register into the existing target crop.

    Reuses the target's HRFFA session, restoring its outputs for the diagnostic
    overlay. Pixels stay on CUDA; only another 544 bytes of landmarks are read.
    No frame history or neutral calibration is retained.
    """
    def __init__(self,cp,landmarker,side,source_points,source_bbox):
        self.cp=cp;self.landmarker=landmarker;self.side=side
        self.source_points=np.asarray(source_points,np.float32)
        self.source_bbox=np.asarray(source_bbox,np.float32)
        self.image=cp.empty((side,side,4),cp.uint8)
        self.output=cp.empty((1,3,side,side),cp.float32)
        self.raw_backup=cp.empty_like(landmarker.model.outputs[0])
        module=cp.RawModule(code=REGISTRATION_KERNELS)
        self.convert=module.get_function('portrait_bgra');self.warp=module.get_function('register_portrait')
        self.clear()
        print('LivePortrait output registration: shared HRFFA, 544 extra metadata bytes, GPU similarity, no calibration',flush=True)

    def clear(self):
        self.applied=False;self.error=None;self.metadata_bytes=0;self.reason='no_target'

    def align(self,pixels,target_points,target_matrix,target_five=None):
        import cv2
        from face_swap_landmarker import five_points
        self.clear()
        if target_points is not None:target_five=five_points(target_points)
        if target_five is None:return None
        cp=self.cp;marker=self.landmarker;side=self.side
        saved_points,saved_inverse=marker.points,marker.inverse
        cp.copyto(self.raw_backup,marker.model.outputs[0])
        try:
            self.convert(((side*side+255)//256,),(256,),(pixels,self.image,np.int32(side)))
            marker.refine(self.image,side,side,self.source_bbox,self.source_points,check_reference=False)
            self.metadata_bytes=marker.metadata_bytes
            if marker.points is None:
                self.reason='generated_landmarks_rejected';return None
            generated=five_points(marker.points)
            target=cv2.transform(np.asarray(target_five,np.float32)[None],np.asarray(target_matrix,np.float32))[0]
            matrix=registration_matrix(generated,target,side)
            if matrix is None:
                self.reason='transform_rejected';return None
            adjusted=cv2.transform(generated[None],matrix)[0]
            before=float(np.sqrt(np.mean(np.sum((generated-target)**2,axis=1))))
            after=float(np.sqrt(np.mean(np.sum((adjusted-target)**2,axis=1))))
            self.error=(round(before,2),round(after,2))
            inverse=cv2.invertAffineTransform(matrix)
            self.warp(((side*side+255)//256,),(256,),
                      (pixels,self.output,np.int32(side),*(np.float32(v) for v in inverse.ravel())))
            self.applied=True;self.reason='aligned_hrffa' if target_points is not None else 'aligned_detector'
            return self.output
        finally:
            marker.points,marker.inverse=saved_points,saved_inverse
            cp.copyto(marker.model.outputs[0],self.raw_backup)


def rotation(xp,pitch,yaw,roll):
    x,y,z=[xp.asarray(value,dtype=xp.float32).reshape(())*np.float32(np.pi/180) for value in (pitch,yaw,roll)]
    one=xp.ones_like(x);zero=xp.zeros_like(x)
    def row(*values):return xp.stack(values)
    rx=xp.stack([row(one,zero,zero),row(zero,xp.cos(x),-xp.sin(x)),row(zero,xp.sin(x),xp.cos(x))])
    ry=xp.stack([row(xp.cos(y),zero,xp.sin(y)),row(zero,one,zero),row(-xp.sin(y),zero,xp.cos(y))])
    rz=xp.stack([row(xp.cos(z),-xp.sin(z),zero),row(xp.sin(z),xp.cos(z),zero),row(zero,zero,one)])
    return (rz@ry@rx).T


def transformed_points(xp,motion):
    r=rotation(xp,motion['pitch'],motion['yaw'],motion['roll'])
    t=motion['translation'].copy();t[...,2]=0
    return motion['scale'].reshape(1,1,1)*(motion['motion_points']@r+motion['expression'])+t.reshape(1,1,3)


# Six rigid anchors from the 68-point topology: nose, chin, eye corners, mouth corners.
POSE_INDICES=np.array([30,8,36,45,48,54])
POSE_MODEL=np.array([[0,0,0],[0,330,65],[-225,-170,135],[225,-170,135],
                     [-150,150,125],[150,150,125]],np.float64)


def landmark_pose(points,matrix):
    """Estimate pose in the same aligned crop as the neural motion extractor."""
    import cv2
    points=np.asarray(points,np.float64)
    if points.shape!=(68,2) or not np.isfinite(points).all():return None
    image=cv2.transform(points[None],np.asarray(matrix,np.float64))[0][POSE_INDICES]
    camera=np.array([[256,0,128],[0,256,128],[0,0,1]],np.float64)
    try:ok,rvec,tvec=cv2.solvePnP(POSE_MODEL,image,camera,None,flags=cv2.SOLVEPNP_ITERATIVE)
    except cv2.error:return None
    if not ok or not np.isfinite(rvec).all() or tvec[2,0]<=0:return None
    projected,_=cv2.projectPoints(POSE_MODEL,rvec,tvec,camera,None)
    if np.sqrt(np.mean((projected.reshape(-1,2)-image)**2))>12:return None
    r,_=cv2.Rodrigues(rvec)
    # Column-camera Euler angles; the animator converts to row rotations once.
    pitch=np.arctan2(r[2,1],r[2,2]);yaw=np.arctan2(-r[2,0],np.hypot(r[2,1],r[2,2]))
    roll=np.arctan2(r[1,0],r[0,0])
    angles=np.rad2deg([pitch,yaw,roll]).astype(np.float32)
    return angles if np.max(np.abs(angles))<85 else None


def driven_points(xp,source,driver,strength,pose=None):
    """Use only the current frame: donor geometry, absolute pose and expression.

    HRFFA pose replaces neural pose. No neutral frame, reference delta or learned
    anchor influences the output. Source tensors are the fixed selected photo.
    """
    amount=np.float32(strength/100)
    target=[driver[key] for key in ('pitch','yaw','roll')] if pose is None else pose
    angles=[source[key]+amount*(value-source[key]) for key,value in zip(('pitch','yaw','roll'),target)]
    r=rotation(xp,*angles)
    expression=source['expression']+amount*(driver['expression']-source['expression'])
    ratio=driver['scale']/xp.maximum(source['scale'],np.float32(1e-4))
    scale=source['scale']*xp.clip(1+amount*(ratio-1),.8,1.2)
    translation=source['translation']+amount*xp.clip(driver['translation']-source['translation'],-.08,.08)
    translation=translation.copy();translation[...,2]=0
    return scale.reshape(1,1,1)*(source['motion_points']@r+expression)+translation.reshape(1,1,3)


def make_portrait_model(cp,path,stream,device,cache,builder_level=5,tensorrt=True):
    from face_swap_gpu_worker import DeviceModel
    if tensorrt:
        try:
            from face_swap_tensorrt import TensorRTModel
            model=TensorRTModel(cp,path,stream,device,Path(cache).parent/'tensorrt-engines',
                                fp16=False,builder_level=builder_level)
            model.run()
            print(f'{Path(path).stem}: TensorRT FP32 ready; builder level {builder_level}',flush=True)
            return model,'TensorRT FP32'
        except Exception as error:
            print(f'{Path(path).stem}: TensorRT level {builder_level} unavailable ({error}); using CUDA',flush=True)
    model=DeviceModel(cp,path,stream,device)
    model.run()
    return model,'CUDA Graph' if model.graph_ready else 'CUDA IO Binding'


class PortraitAnimator:
    def __init__(self,cp,root,stream,device,cache,source_image,source_landmarks,builder_level=5,tensorrt=True):
        import cv2
        from face_swap_gpu_worker import KERNELS
        from face_swap_progress import report
        self.cp=cp
        models=[];backends=[]
        for name in NAMES:
            report('Preparing LivePortrait: '+name.removeprefix('live_portrait_'))
            model,backend=make_portrait_model(cp,Path(root)/(name+'.onnx'),stream,device,cache,builder_level,tensorrt)
            models.append(model);backends.append(backend)
        self.feature,self.motion,self.generator,self.stitch=models
        self.backend='LivePortrait / '+(backends[0] if len(set(backends))==1 else 'mixed GPU')
        self.motion_names=[meta.name for meta in self.motion.session.get_outputs()]
        if set(self.motion_names)!=set(FIELDS):raise ValueError('Unexpected LivePortrait motion outputs')
        self.generator_inputs={meta.name:value for meta,value in zip(self.generator.session.get_inputs(),self.generator.inputs)}
        self.stitch_inputs={meta.name:value for meta,value in zip(self.stitch.session.get_inputs(),self.stitch.inputs)}
        if set(self.generator_inputs)!={'feature_volume','source','target'} or set(self.stitch_inputs)!={'source','target'}:
            raise ValueError('Unexpected LivePortrait generator/stitch inputs')
        self.driver_input=self.motion.inputs[0]
        if self.driver_input.shape!=(1,3,256,256) or self.generator.outputs[0].shape!=(1,3,512,512):
            raise ValueError('LivePortrait requires a 256 RGB crop and 512 RGB output')
        module=cp.RawModule(code=KERNELS);pack=module.get_function('aligned_pack')
        source_gpu=cp.asarray(cv2.cvtColor(source_image,cv2.COLOR_BGR2BGRA))
        matrix=portrait_matrix(source_landmarks,256);inverse=cv2.invertAffineTransform(matrix)
        pack((256,),(256,),(source_gpu,np.int32(source_image.shape[1]),np.int32(source_image.shape[0]),
            self.feature.inputs[0],np.int32(256),*(np.float32(v) for v in inverse.ravel()),
            np.float32(0),np.float32(1/255),np.int32(1)))
        self.feature.run();cp.copyto(self.driver_input,self.feature.inputs[0]);self.motion.run()
        self.source={name:value.copy() for name,value in zip(self.motion_names,self.motion.outputs)}
        self.source_points=transformed_points(cp,self.source)
        self.pose_backend='LivePortrait'
        cp.copyto(self.generator_inputs['feature_volume'],self.feature.outputs[0])
        cp.copyto(self.generator_inputs['target'],self.source_points)
        cp.copyto(self.generator_inputs['source'],self.source_points)
        self.generator.run()
        report('LivePortrait source appearance ready; direct tracking, no calibration')
        print(f'LivePortrait reference animation ready: {self.backend}; source feature volume cached; source canonical points retained',flush=True)

    def animate(self,strength=100,points68=None,matrix=None):
        self.motion.run()
        driver=dict(zip(self.motion_names,self.motion.outputs))
        pose=landmark_pose(points68,matrix) if points68 is not None and matrix is not None else None
        self.pose_backend='HRFFA' if pose is not None else 'LivePortrait'
        desired=driven_points(self.cp,self.source,driver,strength,pose)
        # FaceFusion's pinned exports call the desired/driving points "source"
        # and the appearance/reference points "target". Keep this boundary explicit.
        self.cp.copyto(self.stitch_inputs['source'],desired)
        self.cp.copyto(self.stitch_inputs['target'],self.source_points)
        self.stitch.run()
        self.cp.copyto(self.generator_inputs['source'],self.stitch.outputs[0])
        self.generator.run()
        return self.generator.outputs[0]
