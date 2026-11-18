"""Optional HRFFA 68-point target alignment, GPU pixels and tiny coordinate readback."""
import hashlib
from pathlib import Path
import numpy as np


def crop_matrix(bbox):
    box=np.asarray(bbox,dtype=np.float64)
    if box.shape!=(4,) or not np.isfinite(box).all() or np.any(box[2:]<=box[:2]):
        raise ValueError('Invalid landmarker face box')
    scale=256/(max(float(np.max(box[2:]-box[:2])),1)*1.7)
    shift=128-(box[:2]+box[2:])*.5*scale
    return np.array([[scale,0,shift[0]],[0,scale,shift[1]]],np.float64)


def five_points(points):
    points=np.asarray(points,dtype=np.float32)
    if points.shape!=(68,2) or not np.isfinite(points).all():
        raise ValueError('Expected 68 finite face landmarks')
    return np.stack([points[36:42].mean(0),points[42:48].mean(0),
                     points[30],points[48],points[54]])


def decode_prediction(prediction,bbox,detector_points,check_reference=True):
    """HRFFA has no calibrated confidence here; reject implausible geometry instead."""
    raw=np.asarray(prediction,dtype=np.float32)
    if raw.shape not in ((1,68,2),(68,2)) or not np.isfinite(raw).all():return None
    matrix=crop_matrix(bbox)
    points=(raw.reshape(68,2)*256-matrix[:,2])/matrix[0,0]
    refined=five_points(points)
    detected=np.asarray(detector_points,dtype=np.float32)
    extent=float(np.max(np.asarray(bbox)[2:]-np.asarray(bbox)[:2]))
    eye_distance=float(np.linalg.norm(refined[0]-refined[1]))
    if not .04*extent<eye_distance<1.2*extent:return None
    if detected.shape!=(5,2) or not np.isfinite(detected).all():return None
    if check_reference and np.max(np.linalg.norm(refined-detected,axis=1))>.35*extent:return None
    # Generated portraits rotate away from the source photo's reference points.
    # Keep spatial sanity checks without mistaking a valid pose for a bad face.
    if not check_reference:
        box=np.asarray(bbox)
        if np.any(refined<box[:2]-.75*extent) or np.any(refined>box[2:]+.75*extent):return None
    return points.astype(np.float32)


def static_landmarker(path,cache):
    import onnx
    from uuid import uuid4
    path=Path(path);key=hashlib.sha256(path.read_bytes()).hexdigest()[:24]
    target=Path(cache).parent/'prepared'/('hrffa-'+key+'-static-v1.onnx')
    if not target.is_file():
        model=onnx.load(str(path))
        # Only coordinates are used, not the optional three-channel auxiliary logits.
        del model.graph.output[1:]
        for value,shape in ((model.graph.input[0],(1,3,256,256)),(model.graph.output[0],(1,68,2))):
            value.type.tensor_type.shape.ClearField('dim')
            for size in shape:value.type.tensor_type.shape.dim.add().dim_value=size
        onnx.checker.check_model(model)
        target.parent.mkdir(parents=True,exist_ok=True)
        temp=target.with_name(target.name+'.'+uuid4().hex+'.tmp')
        try:onnx.save(model,str(temp));temp.replace(target)
        finally:temp.unlink(missing_ok=True)
    return target


DRAW=r'''
extern "C" __global__ void draw_landmarks(unsigned char* image,int w,int h,const float* points,
 float inv_scale,float tx,float ty) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=68*25)return;
 int i=k/25,dx=k%5-2,dy=k%25/5-2;if(dx*dx+dy*dy>4)return;
 int x=(int)roundf(points[i*2]*256*inv_scale+tx)+dx;
 int y=(int)roundf(points[i*2+1]*256*inv_scale+ty)+dy;
 if(x<0||y<0||x>=w||y>=h)return;
 image[(y*w+x)*4]=255;image[(y*w+x)*4+1]=200;image[(y*w+x)*4+2]=0;image[(y*w+x)*4+3]=255;
}
'''


class GpuLandmarker:
    def __init__(self,cp,path,stream,device,cache,builder_level=5):
        from face_swap_gpu_worker import DeviceModel,KERNELS
        from face_swap_progress import report
        report('Preparing HRFFA 68-point face landmarker')
        graph=static_landmarker(path,cache)
        self.backend='CUDA IO Binding'
        try:
            from face_swap_tensorrt import TensorRTModel
            self.model=TensorRTModel(cp,graph,stream,device,Path(cache).parent/'tensorrt-engines',
                                     fp16=False,builder_level=builder_level)
            self.model.run();self.backend='TensorRT FP32'
        except Exception as error:
            print(f'HRFFA TensorRT level {builder_level} unavailable ({error}); using CUDA',flush=True)
            self.model=DeviceModel(cp,graph,stream,device)
        if self.model.inputs[0].shape!=(1,3,256,256) or self.model.outputs[0].shape!=(1,68,2):
            raise ValueError('HRFFA expects RGB NCHW 256 and 68 coordinate pairs')
        self.stream=stream;self.points=None;self.inverse=None
        self.module=cp.RawModule(code=KERNELS)
        self.pack=self.module.get_function('aligned_pack')
        self.draw_module=cp.RawModule(code=DRAW);self.draw_kernel=self.draw_module.get_function('draw_landmarks')
        self.metadata_bytes=68*2*4
        self.pinned=cp.cuda.alloc_pinned_memory(self.metadata_bytes)
        self.host=np.frombuffer(self.pinned,dtype=np.float32,count=136).reshape(1,68,2)
        self.model.run()
        if self.backend!='TensorRT FP32':self.backend='CUDA Graph' if self.model.graph_ready else 'CUDA IO Binding'
        print(f'HRFFA landmarker ready: {self.backend}; builder level {getattr(self.model,"builder_level","-")}; 68 points; coordinate readback=544 bytes',flush=True)

    def clear(self):
        self.points=None;self.inverse=None

    def refine(self,image,width,height,bbox,detector_points,check_reference=True):
        import cv2
        self.clear()
        inverse=cv2.invertAffineTransform(crop_matrix(bbox))
        self.pack((256,),(256,),(image,np.int32(width),np.int32(height),self.model.inputs[0],np.int32(256),
            *(np.float32(v) for v in inverse.ravel()),np.float32(127.5),np.float32(1/127.5),np.int32(0)))
        self.model.run()
        self.model.outputs[0].get(stream=self.stream,out=self.host,blocking=True)
        self.points=decode_prediction(self.host,bbox,detector_points,check_reference=check_reference)
        if self.points is None:return detector_points
        self.inverse=inverse
        return five_points(self.points)

    def draw(self,image):
        if self.points is None:return
        h,w=image.shape[:2];inverse=self.inverse
        self.draw_kernel((7,),(256,),(image,np.int32(w),np.int32(h),self.model.outputs[0],
            np.float32(inverse[0,0]),np.float32(inverse[0,2]),np.float32(inverse[1,2])))
