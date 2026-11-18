"""Optional XSeg mask: original BGRA crop -> BGR NHWC -> GPU-only alpha mask."""
import hashlib
from pathlib import Path

KERNELS = r'''
__device__ float byte_sample(const unsigned char* image,int w,int h,float x,float y,int c) {
    x=fminf(w-1.f,fmaxf(0.f,x));y=fminf(h-1.f,fmaxf(0.f,y));
    int ix=(int)x,iy=(int)y,jx=min(ix+1,w-1),jy=min(iy+1,h-1);
    float dx=x-ix,dy=y-iy;
    return (1-dy)*((1-dx)*image[(iy*w+ix)*4+c]+dx*image[(iy*w+jx)*4+c])
         +dy*((1-dx)*image[(jy*w+ix)*4+c]+dx*image[(jy*w+jx)*4+c]);
}
extern "C" __global__ void pack_occlusion(const unsigned char* image,int w,int h,float* out,
    float a,float b,float c,float d,float e,float f) {
    int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=256*256)return;
    int x=k%256,y=k/256;
    for(int ch=0;ch<3;++ch) out[k*3+ch]=byte_sample(image,w,h,a*x+b*y+c,d*x+e*y+f,ch)/255.f;
}
extern "C" __global__ void combine_occlusion(const float* mask,const float* ellipse,float* out,int side) {
    int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;
    float x=fminf(255.f,fmaxf(0.f,((k%side)+.5f)*256/side-.5f));
    float y=fminf(255.f,fmaxf(0.f,((k/side)+.5f)*256/side-.5f));
    int ix=(int)x,iy=(int)y,jx=min(ix+1,255),jy=min(iy+1,255);float dx=x-ix,dy=y-iy;
    float value=(1-dy)*((1-dx)*mask[iy*256+ix]+dx*mask[iy*256+jx])
                  +dy*((1-dx)*mask[jy*256+ix]+dx*mask[jy*256+jx]);
    out[k]=ellipse[k]*fminf(1.f,fmaxf(0.f,(value-.5f)*2));
}
'''


def static_model(path, cache):
    """Freeze batch 1 without changing weights, precision or original ONNX."""
    import onnx
    from uuid import uuid4
    path = Path(path)
    key = hashlib.sha256(path.read_bytes()).hexdigest()[:24]
    target = Path(cache).parent / "prepared" / ("xseg_1-" + key + "-static-v1.onnx")
    if not target.exists():
        model = onnx.load(str(path))
        for value in list(model.graph.input) + list(model.graph.output):
            dim = value.type.tensor_type.shape.dim[0]
            dim.ClearField("dim_param")
            dim.dim_value = 1
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + "." + uuid4().hex + ".tmp")
        try:
            onnx.save(model, str(temporary))
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    return target


class OcclusionMask:
    def __init__(self, cp, path, stream, device, cache, sides, tensorrt=True, builder_level=5):
        from face_swap_gpu_worker import DeviceModel
        from face_swap_progress import report
        report("Preparing XSeg occlusion protection")
        graph = static_model(path, cache)
        self.backend = "CUDA Graph"
        if tensorrt:
            try:
                from face_swap_tensorrt import TensorRTModel
                self.model = TensorRTModel(cp, graph, stream, device, Path(cache).parent / "tensorrt-engines", fp16=False,builder_level=builder_level)
                self.model.run()
                self.backend = "TensorRT FP32"
            except Exception as error:
                print(f"XSeg TensorRT unavailable ({error}); using CUDA IO Binding", flush=True)
                self.model = DeviceModel(cp, graph, stream, device)
        else:
            self.model = DeviceModel(cp, graph, stream, device)
        if self.model.inputs[0].shape != (1,256,256,3) or self.model.outputs[0].shape != (1,256,256,1):
            raise ValueError("XSeg expects BGR NHWC 256x256 and one output mask")
        self.cp = cp
        self.module = cp.RawModule(code=KERNELS)
        self.pack = self.module.get_function("pack_occlusion")
        self.combine_kernel = self.module.get_function("combine_occlusion")
        self.clipped = cp.empty((256,256), dtype=cp.float32)
        self.blurred = cp.empty_like(self.clipped)
        self.combined = {side:cp.empty((side,side),dtype=cp.float32) for side in set(sides) if side}
        self.model.run()
        if self.backend != "TensorRT FP32":
            self.backend = "CUDA Graph" if self.model.graph_ready else "CUDA IO Binding"
        # Compile filtering kernels during preparation, not on the first video frame.
        self.filter()
        print(f"XSeg occlusion ready: {self.backend}; builder level {getattr(self.model,'builder_level','-')}; 256x256; GPU mask; no image readback", flush=True)

    def filter(self):
        from cupyx.scipy.ndimage import gaussian_filter
        self.cp.clip(self.model.outputs[0].reshape(256,256),0,1,out=self.clipped)
        gaussian_filter(self.clipped,5,output=self.blurred,mode="mirror",truncate=3)

    def update(self, image, width, height, matrix, swap_size):
        import cv2
        import numpy as np
        inverse = cv2.invertAffineTransform(matrix * (256 / swap_size))
        self.pack(((256*256+255)//256,), (256,), (image,np.int32(width),np.int32(height),self.model.inputs[0],
                  *(np.float32(value) for value in inverse.ravel())))
        self.model.run()
        self.filter()

    def combine(self, ellipse, side):
        import numpy as np
        result = self.combined[side]
        self.combine_kernel(((side*side+255)//256,),(256,),(self.blurred,ellipse,result,np.int32(side)))
        return result
