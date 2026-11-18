"""Optional BiSeNet region mask on the original face, with GPU-only composition."""
from pathlib import Path
import hashlib
from face_swap_occlusion import KERNELS as OCCLUSION_KERNELS

KERNELS=OCCLUSION_KERNELS+r'''
extern "C" __global__ void pack_parser(const unsigned char* image,int w,int h,float* out,
 float a,float b,float c,float d,float e,float f) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=512*512)return;
 int x=k%512,y=k/512;
 const float mean[3]={.485f,.456f,.406f},scale[3]={.229f,.224f,.225f};
 for(int ch=0;ch<3;++ch) out[ch*512*512+k]=(byte_sample(image,w,h,a*x+b*y+c,d*x+e*y+f,2-ch)/255.f-mean[ch])/scale[ch];
}
extern "C" __global__ void parser_regions(const float* logits,float* mask,unsigned int regions) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=512*512)return;
 int best=0;float score=logits[k];
 for(int c=1;c<19;++c){float v=logits[c*512*512+k];if(v>score){best=c;score=v;}}
 mask[k]=(regions & (1u<<best)) ? 1.f : 0.f;
}
extern "C" __global__ void combine_parser(const float* mask,const float* base,float* out,int side) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;
 float x=fminf(511.f,fmaxf(0.f,((k%side)+.5f)*512/side-.5f));
 float y=fminf(511.f,fmaxf(0.f,((k/side)+.5f)*512/side-.5f));
 int ix=(int)x,iy=(int)y,jx=min(ix+1,511),jy=min(iy+1,511);float dx=x-ix,dy=y-iy;
 float v=(1-dy)*((1-dx)*mask[iy*512+ix]+dx*mask[iy*512+jx])+dy*((1-dx)*mask[jy*512+ix]+dx*mask[jy*512+jx]);
 out[k]=base[k]*fminf(1.f,fmaxf(0.f,(v-.5f)*2));
}
extern "C" __global__ void protect_parser(const float* mask,const float* base,float* out,int side) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;
 float x=fminf(511.f,fmaxf(0.f,((k%side)+.5f)*512/side-.5f));
 float y=fminf(511.f,fmaxf(0.f,((k/side)+.5f)*512/side-.5f));
 int ix=(int)x,iy=(int)y,jx=min(ix+1,511),jy=min(iy+1,511);float dx=x-ix,dy=y-iy;
 float v=(1-dy)*((1-dx)*mask[iy*512+ix]+dx*mask[iy*512+jx])+dy*((1-dx)*mask[jy*512+ix]+dx*mask[jy*512+jx]);
 out[k]=base[k]*(1.f-fminf(1.f,fmaxf(0.f,(v-.5f)*2)));
}
'''


def region_bits(mode):
    if mode not in (1,2,3,4):raise ValueError('BiSeNet mode must be 1..4')
    selected={1,2,3,4,5,6,10,11,12,13}
    if mode in (2,4):selected-={4,5,6}
    if mode in (3,4):selected-={11,12,13}
    return sum(1<<label for label in selected)


def static_parser(path,cache):
    import onnx
    from uuid import uuid4
    path=Path(path);key=hashlib.sha256(path.read_bytes()).hexdigest()[:24]
    target=Path(cache).parent/'prepared'/('bisenet_resnet_18-'+key+'-static-v1.onnx')
    if not target.exists():
        model=onnx.load(str(path))
        # Only the primary logits are used. ORT prunes the unused auxiliary heads.
        del model.graph.output[1:]
        for value,shape in ((model.graph.input[0],(1,3,512,512)),(model.graph.output[0],(1,19,512,512))):
            value.type.tensor_type.shape.ClearField('dim')
            for size in shape:value.type.tensor_type.shape.dim.add().dim_value=size
        target.parent.mkdir(parents=True,exist_ok=True)
        temporary=target.with_name(target.name+'.'+uuid4().hex+'.tmp')
        try:onnx.save(model,str(temporary));temporary.replace(target)
        finally:temporary.unlink(missing_ok=True)
    return target


class RegionMask:
    def __init__(self,cp,path,stream,device,cache,sides,mode=1,tensorrt=True,builder_level=5):
        from face_swap_gpu_worker import DeviceModel
        from face_swap_progress import report
        report('Preparing BiSeNet face regions')
        graph=static_parser(path,cache)
        self.mode=mode
        self.regions=region_bits(mode)
        self.protected_bits=region_bits(1)&~self.regions
        self.protection_ready=False
        self.backend='CUDA IO Binding'
        if tensorrt:
            try:
                from face_swap_tensorrt import TensorRTModel
                self.model=TensorRTModel(cp,graph,stream,device,Path(cache).parent/'tensorrt-engines',fp16=False,builder_level=builder_level)
                self.model.run();self.backend='TensorRT FP32'
            except Exception as error:
                print(f'BiSeNet TensorRT unavailable ({error}); using CUDA IO Binding',flush=True)
                self.model=DeviceModel(cp,graph,stream,device)
        else:self.model=DeviceModel(cp,graph,stream,device)
        if self.model.inputs[0].shape!=(1,3,512,512) or self.model.outputs[0].shape!=(1,19,512,512):
            raise ValueError('BiSeNet expects RGB NCHW 512x512 and 19 classes')
        self.cp=cp;self.module=cp.RawModule(code=KERNELS)
        self.pack=self.module.get_function('pack_parser')
        self.segment=self.module.get_function('parser_regions')
        self.combine_kernel=self.module.get_function('combine_parser')
        self.protect_kernel=self.module.get_function('protect_parser')
        self.mask=cp.empty((512,512),dtype=cp.float32);self.blurred=cp.empty_like(self.mask)
        self.combined={side:cp.empty((side,side),dtype=cp.float32) for side in set(sides) if side}
        if self.protected_bits:
            self.protected_mask=cp.empty_like(self.mask);self.protected_blurred=cp.empty_like(self.mask)
            self.protected_combined={side:cp.empty_like(value) for side,value in self.combined.items()}
        self.model.run();self.filter()
        if self.backend!='TensorRT FP32':self.backend='CUDA Graph' if self.model.graph_ready else 'CUDA IO Binding'
        print(f'BiSeNet regions ready: {self.backend}; builder level {getattr(self.model,"builder_level","-")}; mode={mode}; 512x512; GPU mask; no image readback',flush=True)

    def filter(self):
        import numpy as np
        from cupyx.scipy.ndimage import gaussian_filter
        self.segment((1024,), (256,), (self.model.outputs[0],self.mask,np.uint32(self.regions)))
        gaussian_filter(self.mask,5,output=self.blurred,mode='mirror',truncate=3)

    def update(self,image,width,height,matrix,swap_size):
        import cv2
        import numpy as np
        inverse=cv2.invertAffineTransform(matrix*(512/swap_size))
        self.pack((1024,),(256,),(image,np.int32(width),np.int32(height),self.model.inputs[0],
                  *(np.float32(value) for value in inverse.ravel())))
        self.protection_ready=False
        self.model.run();self.filter()

    def combine(self,base,side):
        import numpy as np
        result=self.combined[side]
        self.combine_kernel(((side*side+255)//256,),(256,),(self.blurred,base,result,np.int32(side)))
        return result

    def protect_preserved(self,base,side):
        if not self.protected_bits:return base
        import numpy as np
        if not self.protection_ready:
            from cupyx.scipy.ndimage import gaussian_filter
            self.segment((1024,),(256,),(self.model.outputs[0],self.protected_mask,np.uint32(self.protected_bits)))
            gaussian_filter(self.protected_mask,5,output=self.protected_blurred,mode='mirror',truncate=3)
            self.protection_ready=True
        result=self.protected_combined[side]
        self.protect_kernel(((side*side+255)//256,),(256,),(self.protected_blurred,base,result,np.int32(side)))
        return result
