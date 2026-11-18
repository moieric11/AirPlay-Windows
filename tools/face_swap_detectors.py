"""Alternative face detectors: fixed ONNX IO, BGR input, shared 5-point contract."""
from pathlib import Path
import hashlib
import numpy as np

KINDS=('scrfd','yoloface','retinaface','yunet')
SIZES={'yoloface':640,'retinaface':320,'yunet':640}


def fixed_model(path,kind,cache):
    import onnx
    from uuid import uuid4
    side=SIZES[kind];path=Path(path)
    key=hashlib.sha256(path.read_bytes()).hexdigest()[:24]
    target=Path(cache)/(f'{kind}-{side}-{key}-static-v1.onnx')
    if not target.exists():
        model=onnx.load(str(path))
        counts=[(side//stride)**2 for stride in (8,16,32)]
        shapes=([(n*2,c) for c in (1,4,10) for n in counts] if kind=='retinaface' else
                [(1,20,8400)] if kind=='yoloface' else [(1,n,c) for c in (1,1,4,10) for n in counts])
        if len(model.graph.output)!=len(shapes):raise ValueError('Unsupported detector output layout: '+kind)
        for tensor,shape in [(model.graph.input[0],(1,3,side,side))]+list(zip(model.graph.output,shapes)):
            tensor.type.tensor_type.shape.ClearField('dim')
            for n in shape:tensor.type.tensor_type.shape.dim.add().dim_value=n
        target.parent.mkdir(parents=True,exist_ok=True)
        temporary=target.with_name(target.name+'.'+uuid4().hex+'.tmp')
        try:onnx.save(model,str(temporary));temporary.replace(target)
        finally:temporary.unlink(missing_ok=True)
    return target


def nms(rows,threshold=.4):
    if not len(rows):return []
    order=np.argsort(rows[:,4])[::-1];keep=[]
    areas=(rows[:,2]-rows[:,0]+1)*(rows[:,3]-rows[:,1]+1)
    while len(order):
        i=order[0];keep.append(i);rest=order[1:]
        x1=np.maximum(rows[i,0],rows[rest,0]);y1=np.maximum(rows[i,1],rows[rest,1])
        x2=np.minimum(rows[i,2],rows[rest,2]);y2=np.minimum(rows[i,3],rows[rest,3])
        intersection=np.maximum(0,x2-x1+1)*np.maximum(0,y2-y1+1)
        overlap=intersection/np.maximum(areas[i]+areas[rest]-intersection,1e-9)
        order=rest[overlap<=threshold]
    return keep


def decode(outputs,kind,side,threshold=.5):
    """Reference CPU decoder; output x1/y1/x2/y2/score/5 xy landmarks."""
    rows=[]
    if kind=='yoloface':
        raw=outputs[0][0].T
        raw=raw[raw[:,4]>=threshold]
        if len(raw):
            box=np.concatenate((raw[:,:2]-raw[:,2:4]/2,raw[:,:2]+raw[:,2:4]/2),axis=1)
            rows.append(np.concatenate((box,raw[:,4:5],raw[:,5:].reshape(-1,5,3)[:,:,:2].reshape(-1,10)),axis=1))
    else:
        for head,stride in enumerate((8,16,32)):
            score=outputs[head].reshape(-1)
            if kind=='yunet':score=score*outputs[head+3].reshape(-1)
            indices=np.flatnonzero(score>=threshold)
            if not len(indices):continue
            anchors=2 if kind=='retinaface' else 1
            grid=np.stack(((indices//anchors)%(side//stride),(indices//anchors)//(side//stride)),axis=1)*stride
            group=3 if kind=='retinaface' else 6
            box=outputs[head+group].reshape(-1,4)[indices]
            points=outputs[head+group+3].reshape(-1,5,2)[indices]*stride+grid[:,None,:]
            if kind=='retinaface':corners=np.concatenate((grid-box[:,:2]*stride,grid+box[:,2:]*stride),axis=1)
            else:
                center=grid+box[:,:2]*stride;size=np.exp(box[:,2:])*stride
                corners=np.concatenate((center-size/2,center+size/2),axis=1)
            rows.append(np.concatenate((corners,score[indices,None],points.reshape(-1,10)),axis=1))
    result=np.concatenate(rows).astype(np.float32) if rows else np.empty((0,15),np.float32)
    return result[np.isfinite(result).all(axis=1)]


def resize_shape(width,height,side):
    ratio=height/width
    return (max(1,int(side/ratio)),side) if ratio>1 else (side,max(1,int(side*ratio)))


def rescale_rows(rows,width,height,rw,rh):
    # Integer letterboxing can round each axis differently; invert both scales.
    rows[:,0:4:2]*=width/rw;rows[:,1:4:2]*=height/rh
    rows[:,5::2]*=width/rw;rows[:,6::2]*=height/rh
    return rows


class SourceDetector:
    def __init__(self,path,kind,providers,options,cache):
        import onnxruntime as ort
        self.kind=kind;self.side=SIZES[kind];self.model_file=str(path)
        self.graph_path=fixed_model(path,kind,cache)
        self.session=ort.InferenceSession(str(self.graph_path),sess_options=options,providers=providers)
        self.det_thresh=.5
    def prepare(self,**kwargs):self.det_thresh=kwargs.get('det_thresh',.5)
    def nms(self,rows):return nms(rows)
    def detect(self,image,max_num=0,input_size=None):
        import cv2
        h,w=image.shape[:2];rw,rh=resize_shape(w,h,self.side)
        canvas=np.zeros((self.side,self.side,3),np.float32)
        canvas[:rh,:rw]=cv2.resize(image,(rw,rh))
        if self.kind=='retinaface':canvas=(canvas-127.5)/128
        elif self.kind=='yoloface':canvas=canvas/255
        feed=np.ascontiguousarray(canvas.transpose(2,0,1)[None])
        outputs=self.session.run(None,{self.session.get_inputs()[0].name:feed})
        rows=decode(outputs,self.kind,self.side,self.det_thresh)
        rescale_rows(rows,w,h,rw,rh)
        rows=rows[nms(rows)]
        if max_num:rows=rows[:max_num]
        return rows[:,:5],rows[:,5:].reshape(-1,5,2)


KERNELS=r'''
__device__ float pixel(const unsigned char* image,int w,int h,int x,int y,int c) {
 return image[(min(max(y,0),h-1)*w+min(max(x,0),w-1))*4+c];
}
extern "C" __global__ void pack_detector(const unsigned char* image,int w,int h,float* out,int side,int rw,int rh,int kind) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;int x=k%side,y=k/side;
 float sx=fminf(w-1.f,fmaxf(0.f,(x+.5f)*w/rw-.5f)),sy=fminf(h-1.f,fmaxf(0.f,(y+.5f)*h/rh-.5f));
 int ix=(int)sx,iy=(int)sy;float dx=sx-ix,dy=sy-iy;
 for(int c=0;c<3;++c) {
  float v=x<rw&&y<rh ? floorf((1-dy)*((1-dx)*pixel(image,w,h,ix,iy,c)+dx*pixel(image,w,h,ix+1,iy,c))+
       dy*((1-dx)*pixel(image,w,h,ix,iy+1,c)+dx*pixel(image,w,h,ix+1,iy+1,c))+.5f) : 0;
  out[c*side*side+k]=kind==1?v/255.f:kind==2?(v-127.5f)/128.f:v;
 }
}
extern "C" __global__ void decode_yolo(const float* raw,int count,float* out) {
 int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;
 float* r=out+i*15;float score=raw[4*count+i];r[4]=score;if(!(score>=.5f))return;
 float cx=raw[i],cy=raw[count+i],w=raw[2*count+i],h=raw[3*count+i];
 r[0]=cx-w*.5f;r[1]=cy-h*.5f;r[2]=cx+w*.5f;r[3]=cy+h*.5f;
 for(int j=0;j<5;++j){r[5+j*2]=raw[(5+j*3)*count+i];r[6+j*2]=raw[(6+j*3)*count+i];}
}
extern "C" __global__ void decode_yunet(const unsigned long long* pointers,float* out) {
 int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=8400)return;
 int head=i<6400?0:i<8000?1:2,offset=head==0?0:head==1?6400:8000;
 int local=i-offset,stride=8<<head,width=640/stride;
 const float* cls=(const float*)pointers[head];const float* obj=(const float*)pointers[head+3];
 float* r=out+i*15;float score=cls[local]*obj[local];r[4]=score;if(!(score>=.5f))return;
 const float* box=(const float*)pointers[head+6]+local*4;
 const float* pts=(const float*)pointers[head+9]+local*10;
 float x=(local%width)*stride,y=(local/width)*stride;
 float cx=box[0]*stride+x,cy=box[1]*stride+y,w=expf(box[2])*stride,h=expf(box[3])*stride;
 r[0]=cx-w*.5f;r[1]=cy-h*.5f;r[2]=cx+w*.5f;r[3]=cy+h*.5f;
 for(int j=0;j<5;++j){r[5+j*2]=pts[j*2]*stride+x;r[6+j*2]=pts[j*2+1]*stride+y;}
}
'''


class Metadata:
    def __init__(self,cp,outputs,kind):
        self.kind=kind;self.outputs=outputs
        self.device=cp.empty((8400,15),cp.float32)
        self.pinned=cp.cuda.alloc_pinned_memory(self.device.nbytes)
        self.host=np.frombuffer(self.pinned,dtype=np.float32,count=8400*15).reshape(8400,15)
        self.module=cp.RawModule(code=KERNELS,options=('--fmad=false',))
        if kind=='yunet':self.pointers=cp.asarray([v.data.ptr for v in outputs],cp.uint64)
        self.kernel=self.module.get_function('decode_yolo' if kind=='yoloface' else 'decode_yunet')
    def download(self,stream):
        if self.kind=='yoloface':self.kernel((33,),(256,),(self.outputs[0],np.int32(8400),self.device))
        else:self.kernel((33,),(256,),(self.pointers,self.device))
        self.device.get(stream=stream,out=self.host,blocking=True)
        return self.host[(self.host[:,4]>=.5)&np.isfinite(self.host).all(axis=1)]


class GpuDetector:
    def __init__(self,cp,source,stream,device,cache,tensorrt,builder_level=5):
        from face_swap_gpu_worker import DeviceModel
        from face_swap_progress import report
        self.kind=source.kind;self.side=source.side;self.stream=stream
        report('Preparing '+self.kind+' face detector')
        self.backend='CUDA'
        if tensorrt:
            try:
                from face_swap_tensorrt import TensorRTModel
                self.model=TensorRTModel(cp,source.graph_path,stream,device,Path(cache).parent/'tensorrt-engines',fp16=False,builder_level=builder_level)
                self.model.run();self.backend='TensorRT FP32'
            except Exception as error:
                print(f'{self.kind} TensorRT unavailable ({error}); using CUDA IO Binding',flush=True)
                self.model=DeviceModel(cp,source.graph_path,stream,device)
        else:self.model=DeviceModel(cp,source.graph_path,stream,device)
        if self.kind=='retinaface':
            from face_swap_detection import DetectionMetadata
            self.metadata=DetectionMetadata(cp,self.model.outputs,(8,16,32),2)
        else:self.metadata=Metadata(cp,self.model.outputs,self.kind)
        self.module=cp.RawModule(code=KERNELS);self.pack=self.module.get_function('pack_detector')
        self.model.run()
        print(f'{self.kind} {self.side}: {self.backend} ready; builder level {getattr(self.model,"builder_level","-")}; GPU pixels; one metadata transfer',flush=True)
    def candidates(self,image,width,height):
        rw,rh=resize_shape(width,height,self.side)
        self.pack(((self.side*self.side+255)//256,),(256,),(image,np.int32(width),np.int32(height),self.model.inputs[0],
                  np.int32(self.side),np.int32(rw),np.int32(rh),np.int32(KINDS.index(self.kind))))
        self.model.run();rows=self.metadata.download(self.stream)
        return rescale_rows(rows,width,height,rw,rh)
