"""GPU-resident image path. Only detection metadata is downloaded per frame.

D3D11 textures are copied device-to-device through the optional driver bridge;
CuPy kernels keep resizing, alignment and compositing on CUDA. ONNX Runtime
binds directly to the persistent CuPy input/output allocations.
"""
import ctypes
import os
from pathlib import Path
import sys
import time


KERNELS = r'''
__device__ float byte_at(const unsigned char* p,int w,int h,int x,int y,int c) {
    return x<0 || y<0 || x>=w || y>=h ? 0.f : (float)p[(y*w+x)*4+c];
}
__device__ float sample_byte(const unsigned char* p,int w,int h,float x,float y,int c) {
    int ix=(int)floorf(x),iy=(int)floorf(y); float dx=x-ix,dy=y-iy;
    return (1-dy)*((1-dx)*byte_at(p,w,h,ix,iy,c)+dx*byte_at(p,w,h,ix+1,iy,c))
           +dy*((1-dx)*byte_at(p,w,h,ix,iy+1,c)+dx*byte_at(p,w,h,ix+1,iy+1,c));
}
__device__ float plane_at(const float* p,int n,int x,int y,int c) {
    return x<0 || y<0 || x>=n || y>=n ? 0.f : p[c*n*n+y*n+x];
}
__device__ float sample_plane(const float* p,int n,float x,float y,int c) {
    int ix=(int)floorf(x),iy=(int)floorf(y);float dx=x-ix,dy=y-iy;
    return (1-dy)*((1-dx)*plane_at(p,n,ix,iy,c)+dx*plane_at(p,n,ix+1,iy,c))
          +dy*((1-dx)*plane_at(p,n,ix,iy+1,c)+dx*plane_at(p,n,ix+1,iy+1,c));
}
extern "C" __global__ void detection_pack(const unsigned char* image,int w,int h,
                    float* out,int n,int rw,int rh) {
    int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=n*n)return;
    int x=k%n,y=k/n;float sx=fminf(w-1.f,fmaxf(0.f,(x+.5f)*w/rw-.5f));
    float sy=fminf(h-1.f,fmaxf(0.f,(y+.5f)*h/rh-.5f));
    for(int c=0;c<3;++c) {
        float value=x<rw && y<rh ? floorf(sample_byte(image,w,h,sx,sy,2-c)+.5f) : 0.f;
        out[c*n*n+k]=(value-127.5f)/128.f;
    }
}
extern "C" __global__ void aligned_pack(const unsigned char* image,int w,int h,
                    float* out,int n,float a,float b,float c,float d,float e,float f,
                    float mean,float scale,int replicate) {
    int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=n*n)return;
    int x=k%n,y=k/n;float sx=a*x+b*y+c,sy=d*x+e*y+f;
    if(replicate){sx=fminf(w-1.f,fmaxf(0.f,sx));sy=fminf(h-1.f,fmaxf(0.f,sy));}
    for(int ch=0;ch<3;++ch) {
        float value=floorf(sample_byte(image,w,h,sx,sy,2-ch)+.5f);
        out[ch*n*n+k]=(value-mean)*scale;
    }
}
extern "C" __global__ void paste(const float* face,const float* mask,int n,
                    unsigned char* image,int w,int h,int x0,int y0,int rw,int rh,
                    float a,float b,float c,float d,float e,float f,float scale,float bias,float strength,float face_zoom) {
    int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=rw*rh)return;
    int x=x0+k%rw,y=y0+k/rw;if(x<0||y<0||x>=w||y>=h)return;
    float sx=a*x+b*y+c,sy=d*x+e*y+f;
    if(sx<0||sy<0||sx>n-1||sy>n-1)return;
    float alpha=fminf(1.f,fmaxf(0.f,sample_plane(mask,n,sx,sy,0)*strength));
    // Scale generated pixels only; target-aligned geometry and semantic masks stay fixed.
    float center=(n-1)*.5f;
    float fx=sx,fy=sy;
    if(face_zoom!=1.f){fx=center+(sx-center)/face_zoom;fy=center+(sy-center)/face_zoom;}
    if(fx<0||fy<0||fx>n-1||fy>n-1)return;
    for(int ch=0;ch<3;++ch) {
        float value=fminf(255.f,fmaxf(0.f,sample_plane(face,n,fx,fy,2-ch)*scale+bias));
        int at=(y*w+x)*4+ch;
        image[at]=(unsigned char)fminf(255.f,fmaxf(0.f,image[at]*(1-alpha)+value*alpha));
    }
    image[(y*w+x)*4+3]=255;
}
'''


def load_cupy(cache):
    # Use the CUDA 12 component wheels already used by ONNX Runtime. No global
    # CUDA/PATH settings are changed outside this child process.
    root = Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cuda_runtime"
    if (root / "include" / "cuda_runtime.h").is_file():
        os.environ["CUDA_PATH"] = str(root)
    os.environ.setdefault("CUPY_CACHE_DIR", str(cache))
    import cupy
    return cupy


class TextureBridge:
    def __init__(self, dll_path, ticket, width, height):
        library = ctypes.CDLL(str(dll_path))
        library.ap_gpu_open.argtypes = [ctypes.c_uint64] * 3 + [ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
        library.ap_gpu_open.restype = ctypes.c_void_p
        for function in (library.ap_gpu_read, library.ap_gpu_write):
            function.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
            function.restype = ctypes.c_int
        library.ap_gpu_close.argtypes = [ctypes.c_void_p]
        library.ap_gpu_error.restype = ctypes.c_char_p
        self.library = library
        self.object = None
        device = ctypes.c_int(-1)
        self.object = library.ap_gpu_open(*ticket, width, height, ctypes.byref(device))
        if not self.object:
            raise RuntimeError(self.error())
        self.device = device.value

    def error(self):
        return (self.library.ap_gpu_error() or b"D3D11/CUDA bridge failed").decode("utf-8", errors="replace")

    def read(self, array, stream):
        if not self.library.ap_gpu_read(self.object, array.data.ptr, stream.ptr):
            raise RuntimeError(self.error())

    def write(self, array, stream):
        if not self.library.ap_gpu_write(self.object, array.data.ptr, stream.ptr):
            raise RuntimeError(self.error())

    def close(self):
        if getattr(self, "object", None):
            self.library.ap_gpu_close(self.object)
            self.object = None

    def __del__(self):
        self.close()


class DeviceModel:
    def __init__(self, cp, path, stream, device, output_shapes=None, input_shapes=None, cuda_graph=None):
        import numpy as np
        import onnxruntime as ort
        self.ort, self.device, self.stream = ort, device, stream
        self.path = path if isinstance(path, bytes) else str(path)
        self.label = Path(path).name if not isinstance(path, bytes) else "SCRFD"
        self.graph_enabled = (os.environ.get("AIRPLAY_FACE_SWAP_CUDA_GRAPHS", "1") != "0"
                              if cuda_graph is None else cuda_graph)
        self.graph_ready = False
        try:
            self.session = self.create_session()
        except Exception:
            if not self.graph_enabled:
                raise
            self.graph_enabled = False
            print(f"GPU {self.label}: graph initialization unavailable; keeping CUDA IO Binding", flush=True)
            self.session = self.create_session()
        self.inputs, self.outputs = [], []
        for index, metadata in enumerate(self.session.get_inputs()):
            shape = input_shapes[index] if input_shapes else metadata.shape
            if metadata.type != "tensor(float)" or any(not isinstance(n, int) or n < 1 for n in shape):
                raise ValueError("GPU path needs fixed float32 model tensors")
            self.inputs.append(cp.zeros(tuple(shape), dtype=cp.float32))
        for index, metadata in enumerate(self.session.get_outputs()):
            shape = output_shapes[index] if output_shapes else metadata.shape
            if metadata.type != "tensor(float)" or any(not isinstance(n, int) or n < 1 for n in shape):
                raise ValueError("GPU path needs fixed float32 model outputs")
            self.outputs.append(cp.zeros(tuple(shape), dtype=cp.float32))
        self.bind()

    def create_session(self):
        ort = self.ort
        options = ort.SessionOptions()
        options.log_severity_level = 3
        options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
        session = ort.InferenceSession(self.path, sess_options=options,
            providers=[("CUDAExecutionProvider", {"device_id": self.device,
                "user_compute_stream": str(self.stream.ptr), "enable_cuda_graph": int(self.graph_enabled)})])
        session.disable_fallback()
        if "CUDAExecutionProvider" not in session.get_providers():
            raise RuntimeError("GPU-resident model could not activate CUDA")
        return session

    def bind(self):
        import numpy as np
        self.binding = self.session.io_binding()
        for metadata, tensor in zip(self.session.get_inputs(), self.inputs):
            self.binding.bind_input(metadata.name, "cuda", self.device, np.float32, tensor.shape, tensor.data.ptr)
        for metadata, tensor in zip(self.session.get_outputs(), self.outputs):
            self.binding.bind_output(metadata.name, "cuda", self.device, np.float32, tensor.shape, tensor.data.ptr)

    def run(self):
        try:
            self.session.run_with_iobinding(self.binding)
        except Exception as error:
            if not self.graph_enabled or self.graph_ready:
                raise
            # Rebuild only the failed capture session. Keep tensor addresses,
            # model precision, adapter and user stream; never enable CPU EP.
            self.stream.synchronize()
            self.graph_enabled = False
            self.binding = self.session = None
            self.session = self.create_session()
            self.bind()
            print(f"GPU {self.label}: capture unavailable ({type(error).__name__}); keeping CUDA IO Binding", flush=True)
            self.session.run_with_iobinding(self.binding)
        if self.graph_enabled and not self.graph_ready:
            self.graph_ready = True
            print(f"GPU {self.label}: CUDA Graph captured; persistent device buffers", flush=True)
        return self.outputs


def make_detector(cp, graph, stream, device, cache, shapes, tensorrt=False, builder_level=5):
    if tensorrt:
        try:
            import hashlib
            from uuid import uuid4
            from face_swap_tensorrt import TensorRTModel
            # Persist the already-specialized 320x320 graph for TensorRT's
            # parser/cache. Never edit the original SCRFD model on disk.
            key = hashlib.sha256(graph).hexdigest()[:24]
            path = Path(cache).parent / "prepared" / ("scrfd-320-"+key+".onnx")
            if not path.is_file():
                path.parent.mkdir(parents=True, exist_ok=True)
                temporary = path.with_name(path.name+"."+uuid4().hex+".tmp")
                try:
                    temporary.write_bytes(graph)
                    temporary.replace(path)
                finally:
                    temporary.unlink(missing_ok=True)
            model = TensorRTModel(cp, path, stream, device, Path(cache).parent / "tensorrt-engines", fp16=False, builder_level=builder_level)
            model.run()
            print(f"SCRFD 320: TensorRT FP32 ready; builder level {builder_level}; persistent GPU buffers", flush=True)
            return model, "TensorRT FP32"
        except Exception as error:
            model = None
            print(f"Detector TensorRT unavailable ({error}); keeping CUDA Graph detection", flush=True)
    return DeviceModel(cp, graph, stream, device,
                       output_shapes=shapes, input_shapes=[(1, 3, 320, 320)]), "CUDA"


def make_restorer(cp, path, stream, device, cache, tensorrt=False, builder_level=5):
    if tensorrt and Path(path).name in ("GPEN-BFR-256.onnx", "GPEN-BFR-512.onnx"):
        try:
            from face_swap_tensorrt import TensorRTModel
            model = TensorRTModel(cp, path, stream, device, Path(cache).parent / "tensorrt-engines", fp16=False, builder_level=builder_level)
            model.run()
            print(f"{Path(path).stem}: TensorRT FP32 ready; builder level {builder_level}; device IO Binding and engine cache", flush=True)
            return model, "TensorRT FP32"
        except Exception as error:
            model = None
            print(f"GPEN TensorRT unavailable ({error}); keeping CUDA Graph restoration", flush=True)
    return DeviceModel(cp, path, stream, device), "CUDA"


def make_swapper(cp, base, stream, device, cache):
    requested = getattr(base, "swapper_backend", "cuda")
    if requested != "cuda":
        builder=getattr(base,"swapper_builder",0)
        if builder not in (0,3,5):
            raise ValueError("Swapper compilation must be automatic (0), 3 or 5")
        # Honour the chosen level; never silently build a lower-level engine.
        level=builder or 5
        try:
            from face_swap_tensorrt import TensorRTModel
            model = TensorRTModel(cp, base.swapper.model_file, stream, device,
                                  Path(cache).parent / "tensorrt-engines", fp16=requested == "tensorrt_fp16",
                                  builder_level=level)
            model.run()
            label = "TensorRT FP16" if requested == "tensorrt_fp16" else "TensorRT FP32"
            print(f"Swapper: {label} ready; builder level {level}", flush=True)
            return model, label
        except Exception as error:
            model = None
            print(f"Swapper TensorRT level {level} unavailable ({error})", flush=True)
        print("Keeping selected swapper on CUDA", flush=True)
    return DeviceModel(cp, base.swapper.model_file, stream, device), "CUDA"


class GpuImageEngine:
    def __init__(self, base, dll_path, ticket, width, height, cache, restorer_path=None, strength=.6, tensorrt=False,
                 detector_tensorrt=None, occluder_path=None, parser_path=None, parser_mode=1, mask_config=None, landmarker_path=None):
        import numpy as np
        builder=getattr(base,"swapper_builder",0)
        if builder not in (0,3,5):raise ValueError("TensorRT compilation must be 3 or 5")
        self.builder_level=builder or 5
        self.cp = cp = load_cupy(cache)
        self.np = np
        # Model shapes are fixed; compilation has no dependency on AirPlay textures.
        self.bridge = TextureBridge(dll_path, ticket, width, height) if ticket is not None else None
        self.device = self.bridge.device if self.bridge else base.device
        cp.cuda.Device(self.device).use()
        self.stream = cp.cuda.Stream(non_blocking=True)
        from contextlib import nullcontext
        self.profile_range = nullcontext
        if os.environ.get("AIRPLAY_FACE_SWAP_NVTX") == "1" and cp.cuda.nvtx.available:
            from cupyx.profiler import time_range
            self.profile_range = time_range  # sync=False: annotations add no GPU waits.
        self.width, self.height = width, height
        self.base = base
        self.metadata_bytes = 0
        self.strength = strength
        self.shape_transfer=None;self.shape_active=False
        self.animation=bool(getattr(base,"animation",False));self.portrait=None;self.portrait_registration=None
        self.dll_path = dll_path
        from face_swap_mask import normalize_settings
        self.mask_config = normalize_settings(mask_config or {})
        with self.stream:
            self.image = cp.empty((height, width, 4), dtype=cp.uint8)
            self.module = cp.RawModule(code=KERNELS, options=("--std=c++11",))
            self.detector_pack = self.module.get_function("detection_pack")
            self.align_pack = self.module.get_function("aligned_pack")
            self.paste_kernel = self.module.get_function("paste")
            from face_swap_mask import MaskPreview
            # Warm these small diagnostic kernels once, so live toggles do not compile.
            self.preview_renderer=MaskPreview(cp,self.mask_config['preview'] or 1)
            self.mask_preview=self.preview_renderer if self.mask_config['preview'] else None
            from face_swap_progress import report
            self.live_detector=None
            if getattr(base,"detector_kind","scrfd")!="scrfd":
                from face_swap_detectors import GpuDetector
                if detector_tensorrt is None:
                    detector_tensorrt=self.animation or tensorrt or getattr(base,"swapper_backend","cuda")!="cuda"
                self.live_detector=GpuDetector(cp,base.detector,self.stream,self.device,cache,detector_tensorrt,builder_level=self.builder_level)
                self.detector=self.live_detector.model
                self.detector_backend=base.detector_kind+" / "+self.live_detector.backend
                self.detection_metadata=self.live_detector.metadata
            else:
                strides = base.detector._feat_stride_fpn
                anchors = base.detector._num_anchors
                if not base.detector.use_kps:
                    raise ValueError("GPU detector needs facial landmarks")
                shapes = [(320 // stride * (320 // stride) * anchors, components)
                          for components in (1, 4, 10) for stride in strides]
                # Freeze SCRFD's spatial dimensions so its shape calculations can
                # be folded rather than assigned to CPU during ORT partitioning.
                from insightface.model_zoo.scrfd import _static_scrfd_model
                detector_model = getattr(base, "_gpu_detector_model", None)
                if detector_model is None:
                    detector_model = _static_scrfd_model(base.detector.model_file, (320, 320), base.detector.session.get_inputs()[0].name)
                    base._gpu_detector_model = detector_model
                if detector_tensorrt is None:
                    detector_tensorrt = self.animation or tensorrt or getattr(base, "swapper_backend", "cuda") != "cuda"
                from face_swap_progress import report
                report("Preparing GPU face detector")
                self.detector, self.detector_backend = make_detector(
                    cp, detector_model, self.stream, self.device, cache, shapes, detector_tensorrt, builder_level=self.builder_level)
                from face_swap_detection import DetectionMetadata
                self.detection_metadata = DetectionMetadata(cp, self.detector.outputs, strides, anchors)
            source_points=base.source.kps
            if self.animation:
                self.swapper=None;self.swap_size=512;self.swap_model='live_portrait'
                self.swap_image_index=0;self.swap_identity_index=None
                self.swap_convention=(0,1/255,255,0)
            else:
                report("Preparing GPU face swapper")
                self.swapper, self.swap_execution = make_swapper(cp, base, self.stream, self.device, cache)
                from face_swap_models import identity_tensor, image_convention
                self.swap_model = getattr(base, "swap_model", "inswapper_128")
                self.swap_image_index = next(i for i, t in enumerate(self.swapper.inputs) if t.ndim == 4)
                self.swap_identity_index = next(i for i, t in enumerate(self.swapper.inputs) if t.ndim == 2)
                self.swap_size = int(self.swapper.inputs[self.swap_image_index].shape[-1])
                self.swap_convention = image_convention(self.swap_model)
                identity = identity_tensor(self.swap_model, base.source, base.swapper)
                self.swapper.inputs[self.swap_identity_index][...] = cp.asarray(identity, dtype=cp.float32)
            self.swap_mask = self.mask(self.swap_size)
            report("Preparing restoration and warming up GPU kernels")
            self.restorer, self.restoration_backend = (make_restorer(cp, restorer_path, self.stream, self.device, cache, tensorrt, builder_level=self.builder_level)
                                                       if restorer_path and not self.animation else (None, "off"))
            self.restore_size = int(self.restorer.inputs[0].shape[-1]) if self.restorer else 0
            self.restore_mask = self.mask(self.restore_size) if self.restorer else None
            from face_swap_mask import MaskExpansion
            self.mask_expander=MaskExpansion(cp,(self.swap_size,self.restore_size))
            self.occluder = None
            if occluder_path:
                from face_swap_occlusion import OcclusionMask
                self.occluder = OcclusionMask(cp, occluder_path, self.stream, self.device, cache,
                                              (self.swap_size,self.restore_size),builder_level=self.builder_level)
            self.parser = None
            if parser_path:
                from face_swap_parser import RegionMask
                self.parser = RegionMask(cp,parser_path,self.stream,self.device,cache,
                                         (self.swap_size,self.restore_size),mode=parser_mode,builder_level=self.builder_level)
            self.landmarker=None;self.landmarker_backend="off"
            if landmarker_path:
                from face_swap_landmarker import GpuLandmarker
                self.landmarker=GpuLandmarker(cp,landmarker_path,self.stream,self.device,cache,builder_level=self.builder_level)
                self.landmarker_backend=self.landmarker.backend
                source_image=getattr(base,'source_image',None)
                if source_image is not None and self.animation:
                    report('Refining source-photo alignment')
                    source_gpu=cp.asarray(base.cv2.cvtColor(source_image,base.cv2.COLOR_BGR2BGRA))
                    self.landmarker.refine(source_gpu,source_image.shape[1],source_image.shape[0],base.source.bbox,base.source.kps)
                    if self.landmarker.points is not None:
                        from face_swap_landmarker import five_points
                        source_points=five_points(self.landmarker.points)
                        print('Source alignment ready: HRFFA 68 landmarks',flush=True)
                    else:print('Source morphology unavailable: HRFFA rejected source geometry',flush=True)
                    del source_gpu
                    self.landmarker.clear()
            if self.animation:
                from face_swap_portrait import PortraitAnimator
                root=Path(cache).parent
                self.portrait=PortraitAnimator(cp,root,self.stream,self.device,cache,base.source_image,source_points,self.builder_level)
                if self.landmarker:
                    from face_swap_portrait import PortraitRegistration,portrait_matrix
                    source_matrix=portrait_matrix(source_points,self.swap_size)
                    crop_points=base.cv2.transform(np.asarray(source_points,np.float32)[None],source_matrix)[0]
                    box=base.source.bbox
                    corners=np.array([[[box[0],box[1]],[box[2],box[1]],[box[2],box[3]],[box[0],box[3]]]],np.float32)
                    corners=base.cv2.transform(corners,source_matrix)[0]
                    crop_box=np.r_[corners.min(0),corners.max(0)]
                    self.portrait_registration=PortraitRegistration(cp,self.landmarker,self.swap_size,crop_points,crop_box)
                self.swapper=self.portrait.motion;self.swap_execution=self.portrait.backend
            self.detector.run()
            self.swapper.run()
            if self.restorer:
                self.restorer.run()
        self.stream.synchronize()
        self.last_face_count = 0;self.last_bbox=None;self.landmarks_refined=False
        print(f"GPU blend mask: {self.mask_config.get('shape','ellipse')}; padding T/R/B/L={self.mask_config.get('padding',(0,0,0,0))}; "
              f"softness={self.mask_config.get('blur',18)}%; preview={self.mask_config.get('preview',0)}; face scale={self.mask_config['face_scale']}%; face size={self.mask_config['face_size']}%; mask expansion={self.mask_config['expansion']}%; alignment={'facefusion' if self.mask_config['alignment'] else 'legacy'}",flush=True)
        print(f"GPU-resident models ready: CUDA device {self.device}; no image readback", flush=True)

    def configure_mask(self, config):
        from face_swap_mask import normalize_settings, geometric_mask
        settings=normalize_settings(config)
        if settings==self.mask_config:return False
        if any(settings[key]!=self.mask_config[key] for key in ('shape','padding','blur')):
            with self.stream:
                masks={side:self.cp.asarray(geometric_mask(side,settings['shape'],settings['padding'],settings['blur']),
                            dtype=self.cp.float32) for side in {self.swap_size,self.restore_size} if side}
            # Only geometric masks change; ORT sessions and all bound IO remain intact.
            self.swap_mask=masks[self.swap_size]
            self.restore_mask=masks.get(self.restore_size)
        self.mask_config=settings
        self.preview_renderer.mode=settings['preview'] or 1
        self.mask_preview=self.preview_renderer if settings['preview'] else None
        print(f"Live blend mask updated: {settings}",flush=True)
        return True

    def mask(self, side):
        from face_swap_mask import geometric_mask
        return self.cp.asarray(geometric_mask(side,self.mask_config.get("shape","ellipse"),
            self.mask_config.get("padding",(0,0,0,0)),self.mask_config.get("blur",18)),dtype=self.cp.float32)

    def blend_mask(self,side):
        mask=self.swap_mask if side==self.swap_size else self.restore_mask
        amount=self.mask_config['expansion']
        if not amount:
            # Keep the previous result exactly when the new setting is zero.
            if self.occluder:mask=self.occluder.combine(mask,side)
            if self.parser:mask=self.parser.combine(mask,side)
            return mask
        if self.occluder:mask=self.occluder.combine(mask,side)
        if self.parser:mask=self.parser.combine(mask,side)
        mask=self.mask_expander.apply(mask,side,amount)
        if self.parser:mask=self.parser.protect_preserved(mask,side)
        # This is deliberate tolerance of the final contour, including XSeg's
        # boundary. Interior occlusions remain, but their edge also moves.
        return mask

    def detect(self):
        cp, np = self.cp, self.np
        w, h = self.width, self.height
        if self.live_detector:
            candidates=self.live_detector.candidates(self.image,w,h)
            self.metadata_bytes+=self.detection_metadata.device.nbytes
        else:
            ratio = h / w
            rw, rh = (max(1, int(320 / ratio)), 320) if ratio > 1 else (320, max(1, int(320 * ratio)))
            self.detector_pack(((320 * 320 + 255) // 256,), (256,),
                (self.image, np.int32(w), np.int32(h), self.detector.inputs[0], np.int32(320), np.int32(rw), np.int32(rh)))
            self.detector.run()
            candidates = self.detection_metadata.download(self.stream)
            self.metadata_bytes += self.detection_metadata.device.nbytes
            candidates[:, :4] /= rh / h
            candidates[:, 5:] /= rh / h
        if not len(candidates):
            self.last_bbox=None
            self.last_face_count = 0
            return None
        candidates = candidates[np.argsort(candidates[:, 4])[::-1]]
        keep = self.base.detector.nms(candidates[:, :5])
        candidates = candidates[keep]
        self.last_face_count = len(candidates)
        areas = (candidates[:, 2] - candidates[:, 0]) * (candidates[:, 3] - candidates[:, 1])
        selected=candidates[np.argmax(areas)]
        self.last_bbox=selected[:4].copy()
        return selected[5:].reshape(5, 2)

    def align(self, landmarks, tensor, side, mean, scale, face_size=100):
        from face_swap_models import estimate_face_matrix
        if self.animation:
            from face_swap_portrait import portrait_matrix
            matrix=portrait_matrix(landmarks,side)
        else:matrix = estimate_face_matrix(landmarks,side,self.mask_config["alignment"])
        from face_swap_mask import resize_face_placement
        matrix = resize_face_placement(matrix, side, face_size)
        inverse = self.base.cv2.invertAffineTransform(matrix)
        np = self.np
        self.align_pack(((side * side + 255) // 256,), (256,),
            (self.image, np.int32(self.width), np.int32(self.height), tensor, np.int32(side),
             *(np.float32(value) for value in inverse.ravel()), np.float32(mean), np.float32(scale), np.int32(self.animation or self.mask_config["alignment"])))
        return matrix

    def paste(self, pixels, mask, side, matrix, scale, bias, strength, face_zoom=1):
        np = self.np
        inverse = self.base.cv2.invertAffineTransform(matrix)
        corners = np.array([[[0, 0], [side, 0], [side, side], [0, side]]], dtype=np.float32)
        corners = self.base.cv2.transform(corners, inverse)[0]
        x0 = max(0, int(np.floor(corners[:, 0].min())))
        x1 = min(self.width, int(np.ceil(corners[:, 0].max())) + 1)
        y0 = max(0, int(np.floor(corners[:, 1].min())))
        y1 = min(self.height, int(np.ceil(corners[:, 1].max())) + 1)
        if x1 <= x0 or y1 <= y0:
            return
        count = (x1 - x0) * (y1 - y0)
        self.paste_kernel(((count + 255) // 256,), (256,),
            (pixels, mask, np.int32(side), self.image, np.int32(self.width), np.int32(self.height),
             np.int32(x0), np.int32(y0), np.int32(x1 - x0), np.int32(y1 - y0),
             *(np.float32(value) for value in matrix.ravel()),
             np.float32(scale), np.float32(bias), np.float32(strength), np.float32(face_zoom)))

    def process(self):
        self.cp.cuda.Device(self.device).use()
        started = time.perf_counter()
        self.metadata_bytes = 0
        if self.portrait_registration:self.portrait_registration.clear()
        with self.profile_range("Faceswap/frame"), self.stream:
            with self.profile_range("Faceswap/read_texture"):
                self.bridge.read(self.image, self.stream)
            uploaded = time.perf_counter()
            with self.profile_range("Faceswap/detect_and_select"):
                landmarks = self.detect()
            detected = time.perf_counter()
            self.landmarks_refined=False
            if self.landmarker:
                self.landmarker.clear()
                if landmarks is not None:
                    with self.profile_range("Faceswap/landmarker"):
                        landmarks=self.landmarker.refine(self.image,self.width,self.height,self.last_bbox,landmarks)
                    self.metadata_bytes+=self.landmarker.metadata_bytes
                    self.landmarks_refined=self.landmarker.points is not None
            landmarked=time.perf_counter()
            parsed = occluded = guided = masked = swapped = landmarked
            debug_mask, debug_matrix = self.swap_mask, None
            self.shape_active=False
            if landmarks is not None:
                mean, scale, output_scale, bias = self.swap_convention
                with self.profile_range("Faceswap/align"):
                    input_side=256 if self.animation else self.swap_size
                    matrix = self.align(landmarks, self.swapper.inputs[self.swap_image_index], input_side, mean, scale)
                    if self.animation:matrix=matrix*(self.swap_size/input_side)
                if self.occluder:
                    with self.profile_range("Faceswap/occlusion"):
                        self.occluder.update(self.image,self.width,self.height,matrix,self.swap_size)
                    occluded = time.perf_counter()
                parsed = occluded
                if self.parser:
                    with self.profile_range("Faceswap/face_regions"):
                        self.parser.update(self.image,self.width,self.height,matrix,self.swap_size)
                    parsed = time.perf_counter()
                guided=time.perf_counter()
                with self.profile_range("Faceswap/blend_mask"):
                    mask=self.blend_mask(self.swap_size)
                masked=time.perf_counter()
                from face_swap_mask import resize_face_placement
                placement = resize_face_placement(matrix, self.swap_size, self.mask_config["face_size"])
                debug_mask, debug_matrix = mask, placement
                with self.profile_range("Faceswap/swapper"):
                    pixels = self.portrait.animate(self.mask_config["animation_strength"],
                        self.landmarker.points if self.landmarker else None,matrix*(256/self.swap_size)) if self.portrait else self.swapper.run()[0]
                if self.portrait_registration:
                    with self.profile_range("Faceswap/portrait_registration"):
                        pixels=self.portrait_registration.align(pixels,self.landmarker.points,matrix,landmarks)
                        self.metadata_bytes+=self.portrait_registration.metadata_bytes
                with self.profile_range("Faceswap/paste"):
                    if pixels is not None:self.paste(pixels, mask, self.swap_size, placement, output_scale, bias, 1, self.mask_config["face_scale"]/100)
                    else:debug_matrix=None
                swapped = time.perf_counter()
                if self.restorer and self.strength > 0:
                    side = self.restore_size
                    matrix = self.align(landmarks, self.restorer.inputs[0], side, 127.5, 1 / 127.5, self.mask_config["face_size"])
                    with self.profile_range("Faceswap/restoration"):
                        pixels = self.restorer.run()[0]
                    restore_mask=self.blend_mask(side)
                    self.paste(pixels, restore_mask, side, matrix, 127.5, 127.5, self.strength)
            restored = time.perf_counter()
            if self.mask_preview:
                with self.profile_range("Faceswap/mask_preview"):
                    self.mask_preview.apply(self.image,debug_mask,self.swap_size,debug_matrix,landmarks)
                    if self.landmarker:self.landmarker.draw(self.image)
            shaped=time.perf_counter()
            with self.profile_range("Faceswap/write_texture_and_wait"):
                self.bridge.write(self.image, self.stream)
        finished = time.perf_counter()
        # Wall-clock stages: the bridge waits for CUDA completion, so write
        # includes any remaining asynchronous kernels. No profiling GPU stalls.
        self.last_timings = {"read": (uploaded-started)*1000, "detect": (detected-uploaded)*1000,
            "landmark": (landmarked-detected)*1000, "occlusion": (occluded-landmarked)*1000, "regions": (parsed-occluded)*1000, "mask":(masked-guided)*1000, "swap": (swapped-masked)*1000, "restore": (restored-swapped)*1000,
            "shape_input":(guided-parsed)*1000, "preview":(shaped-restored)*1000, "write_wait": (finished-shaped)*1000}
        return (finished - started) * 1000

    def replace_textures(self, ticket, width, height):
        replacement = TextureBridge(self.dll_path, ticket, width, height)
        if replacement.device != self.device:
            replacement.close()
            self.cp.cuda.Device(self.device).use()
            return False
        self.cp.cuda.Device(self.device).use()
        self.stream.synchronize()
        if self.bridge:
            self.bridge.close()
        self.bridge = replacement
        self.width, self.height = width, height
        with self.stream:
            self.image = self.cp.empty((height, width, 4), dtype=self.cp.uint8)
        print(f"GPU textures changed to {width}x{height}; model sessions reused", flush=True)
        return True

    def close(self):
        self.cp.cuda.Device(self.device).use()
        self.stream.synchronize()
        if self.bridge:
            self.bridge.close()
        self.detector = self.swapper = self.restorer = self.detection_metadata = self.occluder = self.parser = self.landmarker = self.mask_expander = self.shape_transfer = self.portrait = self.portrait_registration = None
