"""Experimental donor geometry guidance of the GPU swapper input crop."""
import numpy as np

# Eye/lip interior vertices are deliberately omitted: a closed eye/mouth must
# not produce tiny inverted triangles. Expressions remain in the live pixels.
INDICES=np.array(list(range(36))+[36,39,42,45,48,54],np.int32)
ANCHORS=np.array([[-.15,-.15],[.5,-.15],[1.15,-.15],[1.15,.5],
                  [1.15,1.15],[.5,1.15],[-.15,1.15],[-.15,.5]],np.float64)


def pose_weight(points):
    from face_swap_landmarker import five_points
    p=five_points(points).astype(np.float64)
    eyes=p[1]-p[0];distance=float(eyes@eyes)
    if distance<1e-10:return 0.
    yaw=abs(float((p[2]-(p[0]+p[1])*.5)@eyes)/distance)
    return float(np.clip((.4-yaw)/.2,0,1))


def normalize_points(points,alignment):
    import cv2
    from face_swap_landmarker import five_points
    from face_swap_models import estimate_face_matrix
    matrix=estimate_face_matrix(five_points(points),256,alignment)
    return cv2.transform(np.asarray(points,np.float64)[None],matrix)[0]/256


def desired_shape(current,source,reference,amount,preserve_mode=0):
    """Fixed neutral proportions plus current motion; never copy donor eyelid opening."""
    current=np.asarray(current,np.float64)
    result=current.copy()
    # Jaw/nose: fixed calibrated offsets, retaining current articulation.
    for indices in (np.arange(17),np.arange(27,36)):
        result[indices]+=np.clip(source[indices]-reference[indices],-.08,.08)*amount
    for indices in (np.arange(17,22),np.arange(22,27),np.arange(36,42),np.arange(42,48),np.array([48,54])):
        if indices[0]>=36 and indices[0]<48 and preserve_mode in (2,4):continue
        if indices[0]>=48 and preserve_mode in (3,4):continue
        src=source[indices];ref=reference[indices];now=current[indices]
        offset=np.clip(src.mean(0)-ref.mean(0),-.06,.06)
        ratio=np.clip(np.ptp(src[:,0])/max(np.ptp(ref[:,0]),1e-4),.8,1.2)
        result[indices]+=offset*amount
        result[indices,0]+=(now[:,0]-now[:,0].mean())*(ratio-1)*amount
    return result


def triangle_table(current,desired,triangles):
    src=np.concatenate([current[INDICES],ANCHORS])[triangles]
    dst=np.concatenate([desired[INDICES],ANCHORS])[triangles]
    def areas(p):
        a=p[:,1]-p[:,0];b=p[:,2]-p[:,0]
        return a[:,0]*b[:,1]-a[:,1]*b[:,0]
    sa,da=areas(src),areas(dst)
    if np.any(np.abs(sa)<1e-7) or np.any(sa*da<=0) or np.any(np.abs(da/sa)<.2):return None
    hom=np.concatenate([dst,np.ones((*dst.shape[:2],1))],axis=2)
    inverse=np.linalg.inv(hom)
    transform=inverse@src
    table=np.concatenate([dst.min(1),dst.max(1),inverse[:,:,0],inverse[:,:,1],
                          transform[:,:,0],transform[:,:,1]],axis=1)
    return np.ascontiguousarray(table,np.float32)


class ShapeGeometry:
    def __init__(self,source):
        self.source=np.asarray(source,np.float64);self.reset()

    def reset(self):
        self.reference=None;self.triangles=None;self.weight=0.

    def update(self,current,amount,pose=1.,preserve_mode=0):
        from scipy.spatial import Delaunay,QhullError
        current=np.asarray(current,np.float64)
        self.weight=0.
        if current.shape!=(68,2) or not np.isfinite(current).all():return None
        if np.any(current[INDICES]<-.14) or np.any(current[INDICES]>1.14):return None
        if self.reference is None:
            if pose<.8:return None
            try:self.triangles=Delaunay(np.concatenate([current[INDICES],ANCHORS])).simplices
            except QhullError:return None
            self.reference=current.copy()
        if amount<=0 or pose<=0:return None
        weight=float(amount/100)*pose
        for _ in range(5):
            desired=desired_shape(current,self.source,self.reference,weight,preserve_mode)
            if np.max(np.abs(desired-current))<1e-6:return None
            table=triangle_table(current,desired,self.triangles)
            if table is not None:self.weight=weight;return table
            weight*=.5
        return None


KERNELS=r'''
__device__ float plane(const float* p,int side,int x,int y,int ch) {
 x=max(0,min(side-1,x));y=max(0,min(side-1,y));return p[ch*side*side+y*side+x];
}
__device__ float sample_plane(const float* p,int side,float x,float y,int ch) {
 int ix=(int)floorf(x),iy=(int)floorf(y);float dx=x-ix,dy=y-iy;
 return (1-dy)*((1-dx)*plane(p,side,ix,iy,ch)+dx*plane(p,side,ix+1,iy,ch))
       +dy*((1-dx)*plane(p,side,ix,iy+1,ch)+dx*plane(p,side,ix+1,iy+1,ch));
}
extern "C" __global__ void occlusion_weight(const float* mask,float* weight) {
 __shared__ float values[256];int t=threadIdx.x;float sum=0.f;
 const int width=128,height=112,total=width*height;
 for(int k=t;k<total;k+=256)sum+=mask[(k/width+72)*256+k%width+64];
 values[t]=sum;__syncthreads();
 for(int step=128;step>0;step/=2){if(t<step)values[t]+=values[t+step];__syncthreads();}
 if(t==0)weight[0]=fminf(1.f,fmaxf(0.f,(values[0]/total-.6f)/.25f));
}
extern "C" __global__ void guide_crop(const float* src,float* dst,int side,int channels,
 const float* table,int count,const float* weight,int coverage) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;
 float px=(k%side)/(float)side,py=(k/side)/(float)side,sx=px,sy=py;
 if(weight[0]>0)for(int i=0;i<count;++i){
  const float* t=table+i*16;
  if(px<t[0]||py<t[1]||px>t[2]||py>t[3])continue;
  float u=t[4]*px+t[5]*py+t[6],v=t[7]*px+t[8]*py+t[9];
  if(u<-.00001f||v<-.00001f||u+v>1.00001f)continue;
  sx=px+(t[10]*px+t[11]*py+t[12]-px)*weight[0];
  sy=py+(t[13]*px+t[14]*py+t[15]-py)*weight[0];break;
 }
 for(int ch=0;ch<channels;++ch){
  int at=ch*side*side+k;
  float value=weight[0]<=0 ? src[at] : sample_plane(src,side,sx*side,sy*side,ch);
  dst[at]=coverage ? fmaxf(src[at],value) : value;
 }
}
'''


class GpuShapeTransfer:
    def __init__(self,cp,source_points):
        self.cp=cp;self.source_points=np.asarray(source_points,np.float64).copy()
        self.geometry=None;self.alignment=None;self.generation=None;self.input_snapshot=None
        self.active=False;self.count=0;self.source_pose=pose_weight(source_points)
        if self.source_pose<.8:print('Shape guidance needs a more frontal source photo',flush=True)
        self.module=cp.RawModule(code=KERNELS)
        self.kernel=self.module.get_function('guide_crop');self.visibility=self.module.get_function('occlusion_weight')
        self.device_table=cp.empty((256,16),cp.float32);self.weight=cp.ones(1,cp.float32)
        self.mask_buffers={}

    def reset(self):
        if self.geometry:self.geometry.reset()
        self.active=False;self.count=0

    def prepare(self,points,matrix,side,amount,alignment,generation,occluder=None,preserve_mode=0):
        import cv2
        self.active=False;self.count=0
        if points is None:self.reset();return False
        if self.alignment!=alignment:
            self.geometry=ShapeGeometry(normalize_points(self.source_points,alignment));self.alignment=alignment
        if self.generation!=generation:self.reset();self.generation=generation
        normalized=cv2.transform(np.asarray(points,np.float64)[None],matrix)[0]/side
        table=self.geometry.update(normalized,amount,min(pose_weight(points),self.source_pose),preserve_mode)
        if table is None or len(table)>len(self.device_table):return False
        self.count=len(table);self.device_table[:self.count].set(table)
        if occluder is not None:self.visibility((1,),(256,),(occluder.blurred,self.weight))
        else:self.weight.fill(1)
        return True

    def guide_input(self,tensor):
        """Modify the existing ORT-bound input allocation before inference, never video pixels."""
        if not self.count:return False
        side=int(tensor.shape[-1])
        if self.input_snapshot is None or self.input_snapshot.shape!=tensor.shape:
            self.input_snapshot=self.cp.empty_like(tensor)
        self.cp.copyto(self.input_snapshot,tensor)
        self.kernel(((side*side+255)//256,),(256,),(self.input_snapshot,tensor,np.int32(side),np.int32(3),
            self.device_table,np.int32(self.count),self.weight,np.int32(0)))
        self.active=True
        return True

    def coverage(self,mask,side):
        # Cover both the old silhouette and the guided one, including newly
        # reconstructed background in the generated crop when a face narrows.
        if not self.active:return mask
        if side not in self.mask_buffers:self.mask_buffers[side]=self.cp.empty_like(mask)
        result=self.mask_buffers[side]
        self.kernel(((side*side+255)//256,),(256,),(mask,result,np.int32(side),np.int32(1),
            self.device_table,np.int32(self.count),self.weight,np.int32(1)))
        return result
