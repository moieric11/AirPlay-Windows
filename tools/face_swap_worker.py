"""Optional local CUDA, DirectML or CPU face-swap worker. See docs/FACE_SWAP.md for setup.

Wire format: network-order uint32. Greeting [FSW1, UTF-8 path length], path;
reply [FSW1 or 0 on error, message length], message. Frames [FSW1,w,h,bytes]
followed by packed BGR24; replies echo the header and contain swapped BGR24.
No models or images are downloaded by this worker.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import socket
import struct
import sys
import time
from face_swap_progress import configure, report

MAGIC = 0x46535731
GPU_MAGIC = 0x46535732
GPU_ERROR = 0x46535733
MASK_CONFIG = 0x46535734
MAX_SIDE = 1920
HEADER = struct.Struct("!4I")
GREETING = struct.Struct("!2I")


def read_exact(connection, length):
    data = bytearray(length)
    view = memoryview(data)
    offset = 0
    while offset < length:
        received = connection.recv_into(view[offset:])
        if not received:
            raise EOFError("Client disconnected")
        offset += received
    return data


def frame_header(raw):
    magic, width, height, size = HEADER.unpack(raw)
    if (magic != MAGIC or not 2 <= width <= MAX_SIDE
            or not 2 <= height <= MAX_SIDE or width % 2 or height % 2
            or size != width * height * 3):
        raise ValueError("Invalid frame header")
    return width, height, size


def largest_face(faces):
    return max(faces, key=lambda face: max(0, face.bbox[2] - face.bbox[0])
               * max(0, face.bbox[3] - face.bbox[1]), default=None)


def require_provider(models, backend):
    if backend == "cuda":
        return require_cuda(models)
    if backend == "directml":
        for model in models:
            if "DmlExecutionProvider" not in model.session.get_providers():
                raise RuntimeError("A model could not activate DirectML. Check the selected adapter and driver.")
            model.session.disable_fallback()
        return
    for model in models:
        if model.session.get_providers() != ["CPUExecutionProvider"]:
            raise RuntimeError("CPU mode must use CPUExecutionProvider exclusively")
        model.session.disable_fallback()


def require_cuda(models):
    for model in models:
        if "CUDAExecutionProvider" not in model.session.get_providers():
            raise RuntimeError("A model could not activate CUDA. Check NVIDIA/CUDA/cuDNN installation.")
        # Do not silently recreate a failed CUDA session on CPU.
        model.session.disable_fallback()


def execution_config(ort, backend, device):
    options = ort.SessionOptions()
    if backend == "directml":
        if "DmlExecutionProvider" not in ort.get_available_providers():
            raise RuntimeError("DirectML provider unavailable. Use a separate Python environment with onnxruntime-directml.")
        options.enable_mem_pattern = False
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        providers = [("DmlExecutionProvider", {"device_id": device}), "CPUExecutionProvider"]
    elif backend == "cpu":
        options.intra_op_num_threads = min(8, max(1, (os.cpu_count() or 2) // 2))
        providers = ["CPUExecutionProvider"]
    elif backend == "cuda":
        if "CUDAExecutionProvider" not in ort.get_available_providers():
            raise RuntimeError("CUDA provider unavailable. Install onnxruntime-gpu and its CUDA dependencies.")
        providers = [("CUDAExecutionProvider", {"device_id": device}), "CPUExecutionProvider"]
    else:
        raise ValueError("Unsupported execution backend")
    options.log_severity_level = 3
    return providers, options


def directml_detector_session(model_file, input_size, reference_session):
    # InsightFace 2.1 rewrites SCRFD IO shapes for each detection resolution.
    # Disable runtime provider fallback on these derived sessions as well.
    from insightface.model_zoo.scrfd import _default_resolution_session_factory
    session = _default_resolution_session_factory(model_file, input_size, reference_session)
    if "DmlExecutionProvider" not in session.get_providers():
        raise RuntimeError("Detector resolution session lost DirectML")
    session.disable_fallback()
    return session


class FaceRestorer:
    """CUDA GPEN on the swapped face crop, blended only inside its face mask."""
    def __init__(self, path, providers, options, strength, cuda_graph=True):
        import cv2
        import numpy as np
        import onnxruntime as ort
        self.cv2, self.np = cv2, np
        self.cpu = providers == ["CPUExecutionProvider"]
        self.backend = "cpu" if self.cpu else "directml" if any(isinstance(p, tuple) and p[0] == "DmlExecutionProvider" for p in providers) else "cuda"
        cuda_graph = cuda_graph and self.backend == "cuda"
        self.strength = max(0.0, min(1.0, float(strength)))
        gpu_options = {}
        for provider in providers:
            if isinstance(provider, tuple) and provider[0] == "CUDAExecutionProvider":
                gpu_options.update(provider[1])
        self.device = int(gpu_options.get("device_id", 0))
        gpu_options["enable_cuda_graph"] = int(cuda_graph)
        restorer_providers = providers if self.backend != "cuda" else [("CUDAExecutionProvider", gpu_options), "CPUExecutionProvider"]
        self.session = ort.InferenceSession(str(path), providers=restorer_providers, sess_options=options)
        inputs = self.session.get_inputs()
        if len(inputs) != 1 or inputs[0].type != "tensor(float)":
            raise ValueError("GPEN restorer must have one float32 image input")
        shape = inputs[0].shape
        if len(shape) != 4 or shape[1] != 3 or shape[2] not in (256, 512) or shape[3] != shape[2]:
            raise ValueError("Use GPEN-BFR-256.onnx or GPEN-BFR-512.onnx")
        self.size = shape[2]
        self.input_name = inputs[0].name
        self.cpu_input = np.empty((1, 3, self.size, self.size), dtype=np.float32)
        self.binding = self.gpu_input = self.gpu_output = None
        self.mask = np.zeros((self.size, self.size), dtype=np.float32)
        cv2.ellipse(self.mask, (self.size // 2, self.size // 2),
                    (int(self.size * .39), int(self.size * .46)), 0, 0, 360, 1.0, -1)
        self.mask = cv2.GaussianBlur(self.mask, (0, 0), self.size * .025)
        require_provider([self], self.backend)
        # Run the first expensive CUDA allocation before advertising readiness.
        try:
            self.infer(np.zeros((self.size, self.size, 3), dtype=np.uint8))
        except Exception:
            if not cuda_graph:
                raise
            # Some model exports leave nodes on CPU and cannot be captured.
            # Retain CUDA and the same FP32 model; only graph replay is optional.
            self.binding = self.gpu_input = self.gpu_output = None
            self.session = None
            gpu_options["enable_cuda_graph"] = 0
            self.session = ort.InferenceSession(str(path), providers=[("CUDAExecutionProvider", gpu_options),
                                               "CPUExecutionProvider"], sess_options=options)
            require_cuda([self])
            self.infer(np.zeros((self.size, self.size, 3), dtype=np.uint8))
            cuda_graph = False
        print(f"Face restoration: GPEN {self.size}, blend {self.strength:.0%}, backend={self.backend}, CUDA Graph={cuda_graph}", flush=True)

    def infer(self, crop):
        np = self.np
        # Stable host/device allocations allow CUDA Graph replay. Only the
        # aligned face (not the full video image) crosses the device boundary.
        np.copyto(self.cpu_input[0], crop[:, :, ::-1].transpose(2, 0, 1), casting="unsafe")
        np.divide(self.cpu_input, 127.5, out=self.cpu_input)
        np.subtract(self.cpu_input, 1.0, out=self.cpu_input)
        if self.backend != "cuda":
            output = self.session.run(None, {self.input_name: self.cpu_input})[0]
        else:
            output = self.infer_cuda()
        if output.shape != (1, 3, self.size, self.size) or not np.isfinite(output).all():
            raise ValueError("GPEN returned invalid pixels")
        rgb = np.clip((output[0].transpose(1, 2, 0) + 1.0) * 127.5, 0, 255)
        return rgb[:, :, ::-1].astype(np.uint8)

    def infer_cuda(self):
        np = self.np
        if self.binding is None:
            import onnxruntime as ort
            self.gpu_input = ort.OrtValue.ortvalue_from_numpy(self.cpu_input, "cuda", self.device)
            self.gpu_output = ort.OrtValue.ortvalue_from_shape_and_type(
                [1, 3, self.size, self.size], np.float32, "cuda", self.device)
            self.binding = self.session.io_binding()
            self.binding.bind_ortvalue_input(self.input_name, self.gpu_input)
            self.binding.bind_ortvalue_output(self.session.get_outputs()[0].name, self.gpu_output)
        else:
            self.gpu_input.update_inplace(self.cpu_input)
        self.session.run_with_iobinding(self.binding)
        return self.binding.copy_outputs_to_cpu()[0]

    def restore(self, image, landmarks):
        if self.strength == 0:
            return image
        from insightface.utils.face_align import norm_crop2
        cv2, np = self.cv2, self.np
        crop, transform = norm_crop2(image, landmarks, self.size)
        restored = self.infer(crop)
        inverse = cv2.invertAffineTransform(transform)
        corners = np.array([[[0, 0], [self.size, 0], [self.size, self.size], [0, self.size]]], dtype=np.float32)
        corners = cv2.transform(corners, inverse)[0]
        height, width = image.shape[:2]
        x0 = max(0, int(np.floor(corners[:, 0].min())))
        y0 = max(0, int(np.floor(corners[:, 1].min())))
        x1 = min(width, int(np.ceil(corners[:, 0].max())))
        y1 = min(height, int(np.ceil(corners[:, 1].max())))
        if x1 <= x0 or y1 <= y0:
            return image
        inverse[:, 2] -= [x0, y0]
        size = (x1 - x0, y1 - y0)
        detail = cv2.warpAffine(restored, inverse, size, flags=cv2.INTER_LINEAR)
        alpha = cv2.warpAffine(self.mask, inverse, size, flags=cv2.INTER_LINEAR)
        alpha = np.clip(alpha * self.strength, 0, 1)[:, :, None]
        original = image[y0:y1, x0:x1]
        image[y0:y1, x0:x1] = np.clip(original * (1 - alpha) + detail * alpha, 0, 255).astype(np.uint8)
        return image


class CapturedSwapper:
    """Serial, fixed-shape CUDA replay for InsightFace's two-input swap model."""
    def __init__(self, session, device):
        import onnxruntime as ort
        import numpy as np
        self.ort, self.np = ort, np
        self.session, self.device = session, device
        self.inputs = self.outputs = self.binding = self.output_buffers = None
        self.active = True
        self.configure(True)

    def configure(self, capture):
        self.session.set_providers([("CUDAExecutionProvider", {"device_id": self.device,
                                    "enable_cuda_graph": int(capture)}), "CPUExecutionProvider"])
        self.session.disable_fallback()
        if "CUDAExecutionProvider" not in self.session.get_providers():
            raise RuntimeError("Swap session lost its CUDA provider")

    def get_providers(self):
        return self.session.get_providers()

    def disable_fallback(self):
        self.session.disable_fallback()

    def run(self, output_names, values):
        if not self.active:
            return self.session.run(output_names, values)
        try:
            first_run = self.binding is None
            if self.binding is None:
                self.binding = self.session.io_binding()
                self.inputs = {name: self.ort.OrtValue.ortvalue_from_numpy(
                    self.np.ascontiguousarray(value), "cuda", self.device) for name, value in values.items()}
                for name, buffer in self.inputs.items():
                    self.binding.bind_ortvalue_input(name, buffer)
                self.outputs = []
                self.output_buffers = []
                for metadata in self.session.get_outputs():
                    if metadata.type != "tensor(float)" or any(not isinstance(dim, int) or dim < 1 for dim in metadata.shape):
                        raise ValueError("Swapper output is not a fixed float32 tensor")
                    buffer = self.ort.OrtValue.ortvalue_from_shape_and_type(metadata.shape, self.np.float32, "cuda", self.device)
                    self.binding.bind_ortvalue_output(metadata.name, buffer)
                    self.outputs.append(metadata.name)
                    self.output_buffers.append(buffer)
            else:
                for name, value in values.items():
                    self.inputs[name].update_inplace(self.np.ascontiguousarray(value))
            self.session.run_with_iobinding(self.binding)
            if first_run:
                print("Swap inference: CUDA Graph captured, persistent GPU buffers", flush=True)
            arrays = self.binding.copy_outputs_to_cpu()
            return arrays if output_names is None else [arrays[self.outputs.index(name)] for name in output_names]
        except Exception as error:
            self.binding = self.inputs = self.outputs = self.output_buffers = None
            self.active = False
            self.configure(False)
            print(f"Swap CUDA Graph unavailable; keeping standard CUDA: {type(error).__name__}", flush=True)
            return self.session.run(output_names, values)


class CudaEngine:
    def __init__(self, detector, recognizer, swapper, device, restorer=None, restore_strength=.6, roi_swap=True, swap_graph=True,
                 swap_model="inswapper_128", model_cache=None, swapper_backend="cuda", backend="cuda", swapper_builder=5, detector_kind="scrfd", animation=False):
        if backend not in ("cuda", "cpu", "directml"):
            raise ValueError("Unsupported execution backend")
        self.backend = backend
        self.animation=animation
        if animation and backend!="cuda":raise ValueError("LivePortrait animation requires the CUDA GPU-resident path")
        self.device = device
        self.dll_directories = []
        if backend == "cuda" and os.name == "nt":
            # cuDNN loads additional engines on first inference. Older ORT
            # preload_dlls lists omit newer cuDNN sublibraries, so their bin
            # directories must remain visible for the worker's whole lifetime.
            nvidia = Path(sys.prefix) / "Lib" / "site-packages" / "nvidia"
            bins = sorted(path for path in nvidia.glob("*/bin") if path.is_dir())
            if bins:
                os.environ["PATH"] = os.pathsep.join(map(str, bins)) + os.pathsep + os.environ.get("PATH", "")
                self.dll_directories = [os.add_dll_directory(str(path)) for path in bins]
        import onnxruntime as ort
        if backend == "cuda" and hasattr(ort, "preload_dlls"):
            ort.preload_dlls()
        providers, options = execution_config(ort, backend, device)
        import cv2
        import numpy as np
        from insightface.model_zoo import get_model
        from insightface.app.common import Face
        self.cv2, self.np, self.Face = cv2, np, Face
        settings = {"providers": providers, "download": False, "sess_options": options}
        detector_settings = {"static_shape_sessions": backend == "directml"}
        if backend == "directml":
            detector_settings["resolution_session_factory"] = directml_detector_session
        self.detector_kind=detector_kind
        if detector_kind=="scrfd":
            self.detector = get_model(str(detector), **settings, **detector_settings)
        else:
            from face_swap_detectors import SourceDetector
            self.detector=SourceDetector(detector,detector_kind,providers,options,
                model_cache or Path(detector).parent/"prepared")
        self.recognizer = None if animation else get_model(str(recognizer), **settings)
        self.swap_model = swap_model
        if swapper_builder not in (0,3,5):
            raise ValueError("Swapper compilation must be automatic (0), 3 or 5")
        self.swapper_builder = swapper_builder
        self.swapper_backend = swapper_backend
        if animation:
            self.swapper=None
        elif swap_model == "inswapper_128":
            self.swapper = get_model(str(swapper), **settings)
        else:
            from face_swap_models import AlternativeSwapper, prepare_model
            prepared = prepare_model(swap_model, swapper, model_cache or Path(swapper).parent / "prepared", backend=backend)
            self.swapper = AlternativeSwapper(swap_model, prepared, providers, options)
        self.models = (self.detector,) if animation else (self.detector, self.recognizer, self.swapper)
        if any(model is None for model in self.models):
            raise RuntimeError("Unrecognized ONNX model; use compatible detector, ArcFace and swapper weights")
        if not animation and backend == "cuda" and swap_graph and swap_model == "inswapper_128":
            self.swapper.session = CapturedSwapper(self.swapper.session, device)
        require_provider(self.models, self.backend)
        self.detector.prepare(ctx_id=-1 if backend == "cpu" else device, input_size=(320, 320), det_thresh=0.5)
        if self.recognizer:self.recognizer.prepare(ctx_id=-1 if backend == "cpu" else device)
        self.restorer = FaceRestorer(restorer, providers, options, restore_strength) if restorer else None
        if self.restorer:
            self.models += (self.restorer,)
        require_provider(self.models, self.backend)
        self.source = None
        self.last_face_count = 0
        self.last_timings = {}
        self.roi_swap = roi_swap

    def swap_face(self, image, face):
        if not self.roi_swap:
            return self.swapper.get(image, face, self.source, paste_back=True)
        from insightface.utils.face_align import estimate_norm
        cv2, np = self.cv2, self.np
        side = self.swapper.input_size[0]
        matrix = estimate_norm(face.kps, image_size=side)
        inverse = cv2.invertAffineTransform(matrix)
        corners = np.array([[[0, 0], [side, 0], [side, side], [0, side]]], dtype=np.float32)
        corners = cv2.transform(corners, inverse)[0]
        if not np.isfinite(corners).all():
            return image
        # Include the swapper's erosion/feathering halo as well as the aligned
        # square. Its original blending code then processes just this region.
        padding = int(np.max(np.ptp(corners, axis=0)) * .2) + 16
        height, width = image.shape[:2]
        x0 = max(0, int(np.floor(corners[:, 0].min())) - padding)
        y0 = max(0, int(np.floor(corners[:, 1].min())) - padding)
        x1 = min(width, int(np.ceil(corners[:, 0].max())) + padding)
        y1 = min(height, int(np.ceil(corners[:, 1].max())) + padding)
        if x1 <= x0 or y1 <= y0:
            return image
        landmarks = face.kps.copy()
        landmarks -= [x0, y0]
        local_face = self.Face(kps=landmarks)
        image[y0:y1, x0:x1] = self.swapper.get(
            image[y0:y1, x0:x1], local_face, self.source, paste_back=True)
        return image

    def faces(self, image, source=False):
        boxes, landmarks = self.detector.detect(image, max_num=0, input_size=(640, 640) if source else (320, 320))
        if len(boxes) and landmarks is None:
            raise RuntimeError("Detector must provide five facial landmarks")
        return [self.Face(bbox=box[:4], kps=landmarks[index], det_score=box[4])
                for index, box in enumerate(boxes)]

    def set_source(self, path):
        # imdecode + fromfile supports Windows paths containing accents.
        photo = Path(path).expanduser()
        if not photo.is_file() or photo.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("Choose a local photo smaller than 32 MiB")
        image = self.cv2.imdecode(self.np.fromfile(str(photo), dtype=self.np.uint8), self.cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError("Cannot decode source photo")
        faces = self.faces(image, source=True)
        face = largest_face(faces)
        print(f"Source photo: {len(faces)} face(s) detected", flush=True)
        if face is None:
            raise ValueError("No face detected in source photo")
        if self.recognizer:self.recognizer.get(image, face)
        self.source = face
        self.source_image=image # One original photo retained for optional geometry preparation.
        # Warm up the swapper and verify sessions before claiming CUDA ready.
        if self.swapper:self.swapper.get(image, face, face, paste_back=True)
        require_provider(self.models, self.backend)

    def swap(self, payload, width, height):
        if self.animation:raise RuntimeError("LivePortrait cannot fall back to CPU pixel transport; enable CUDA GPU video decoding")
        started = time.perf_counter()
        image = self.np.frombuffer(payload, dtype=self.np.uint8).reshape(height, width, 3).copy()
        faces = self.faces(image)
        detected = time.perf_counter()
        self.last_face_count = len(faces)
        face = largest_face(faces)
        if face is not None:
            image = self.swap_face(image, face)
            swapped = time.perf_counter()
            if self.restorer:
                image = self.restorer.restore(image, face.kps)
        else:
            swapped = time.perf_counter()
        restored = time.perf_counter()
        self.last_timings = {"detect": (detected - started) * 1000, "swap": (swapped - detected) * 1000,
                             "restore": (restored - swapped) * 1000}
        return self.np.ascontiguousarray(image, dtype=self.np.uint8).tobytes()


def serve_client(connection, engine, gpu_config=None):
    magic, length = GREETING.unpack(read_exact(connection, GREETING.size))
    if magic != MAGIC or not 0 < length <= 32768:
        raise ValueError("Invalid source header")
    source = read_exact(connection, length).decode("utf-8")
    try:
        print("Receiver connected; loading source photo...", flush=True)
        report("Detecting the source face and calculating identity")
        engine.set_source(source)
    except Exception as error:
        print(f"Source photo rejected: {error}", file=sys.stderr, flush=True)
        message = str(error).encode("utf-8")[:4096]
        connection.sendall(GREETING.pack(0, len(message)) + message)
        return
    gpu = None
    if gpu_config and gpu_config.get("prepare"):
        try:
            from face_swap_gpu_worker import GpuImageEngine
            report("Preparing GPU models without screen mirroring")
            gpu = GpuImageEngine(engine, gpu_config["dll"], None, 2, 2,
                                 gpu_config["cache"], gpu_config["restorer"], gpu_config["strength"],
                                 tensorrt=gpu_config.get("tensorrt", False),
                                 **({"landmarker_path":gpu_config["landmarker"]} if gpu_config.get("landmarker") else {}),
                                 **({"occluder_path":gpu_config["occluder"]} if gpu_config.get("occluder") else {}),
                                 **({"parser_path":gpu_config["parser"],"parser_mode":gpu_config.get("parser_mode",1)} if gpu_config.get("parser") else {}),
                                 **({"mask_config":gpu_config["mask_config"]} if gpu_config.get("mask_config") else {}))
        except Exception as error:
            print(f"GPU preparation failed: {error}", file=sys.stderr, flush=True)
            message = str(error).encode("utf-8")[:4096]
            connection.sendall(GREETING.pack(0, len(message)) + message)
            return
    backend = getattr(engine, "backend", "cuda").upper()
    message = (backend + (" ready; GPU_TEXTURES" if gpu_config else " ready") +
               ("; GPU_MODELS_READY" if gpu else "")).encode("ascii")
    try:
        connection.sendall(GREETING.pack(MAGIC, len(message)) + message)
        print(f"Source ready; {backend} inference enabled", flush=True)
        report("Models ready; waiting for screen mirroring")
        last_report = 0.0
        gpu_key = None
        while True:
            header = read_exact(connection, HEADER.size)
            kind, width, height, size = HEADER.unpack(header)
            if kind == MASK_CONFIG:
                if not gpu_config or width or height or size not in (28,32,36,40,44,52,56):
                    raise ValueError('Invalid live mask configuration')
                values=struct.unpack('!'+str(size//4)+'I',read_exact(connection,size))
                shape,blur,preview,top,right,bottom,left=values[:7]
                face_scale=values[7] if size>=32 else 100
                face_size=values[8] if size>=36 else 100
                alignment=values[9] if size>=40 else 0
                expansion=values[10] if size>=44 else 0
                shape_transfer=values[11] if size>=52 else 0
                shape_reset=values[12] if size>=52 else 0
                animation_strength=values[13] if size==56 else 100
                if shape not in (0,1):raise ValueError('Invalid live mask shape')
                from face_swap_mask import normalize_settings
                settings=normalize_settings({'shape':'box' if shape else 'ellipse','blur':blur,
                    'preview':preview,'padding':(top,right,bottom,left),'face_scale':face_scale,'face_size':face_size,'alignment':alignment,'expansion':expansion,'shape_transfer':shape_transfer,'shape_reset':shape_reset,'animation_strength':animation_strength})
                if gpu:gpu.configure_mask(settings)
                gpu_config['mask_config']=settings
                connection.sendall(HEADER.pack(MASK_CONFIG,0,0,0))
                continue
            if kind == GPU_MAGIC:
                if not gpu_config or size != 24 or not 2 <= width <= MAX_SIDE or not 2 <= height <= MAX_SIDE:
                    raise ValueError("Invalid GPU frame request")
                ticket = struct.unpack("!3Q", read_exact(connection, 24))
                try:
                    key = (ticket, width, height)
                    if key != gpu_key:
                        reused = gpu and gpu.replace_textures(ticket, width, height)
                        if gpu and not reused:
                            gpu.close()
                            gpu = None
                            gpu_key = None
                        if not reused:
                            from face_swap_gpu_worker import GpuImageEngine
                            gpu = GpuImageEngine(engine, gpu_config["dll"], ticket, width, height,
                                                 gpu_config["cache"], gpu_config["restorer"], gpu_config["strength"],
                                                 **({"tensorrt": True} if gpu_config.get("tensorrt") else {}),
                                                 **({"landmarker_path":gpu_config["landmarker"]} if gpu_config.get("landmarker") else {}),
                                 **({"occluder_path":gpu_config["occluder"]} if gpu_config.get("occluder") else {}),
                                 **({"parser_path":gpu_config["parser"],"parser_mode":gpu_config.get("parser_mode",1)} if gpu_config.get("parser") else {}),
                                 **({"mask_config":gpu_config["mask_config"]} if gpu_config.get("mask_config") else {}))
                        gpu_key = key
                    elapsed = gpu.process()
                    connection.sendall(HEADER.pack(GPU_MAGIC, width, height, 0))
                    now = time.perf_counter()
                    if now - last_report >= 2 or elapsed >= 250:
                        stages = ", ".join(f"{name}={duration:.1f}" for name, duration in getattr(gpu, "last_timings", {}).items())
                        print(f"GPU resident {width}x{height}: {elapsed:.1f} ms; target faces: {gpu.last_face_count}; "
                              f"image readbacks=0; image socket bytes=0; metadata bytes={gpu.metadata_bytes}; "
                              f"swapper={getattr(gpu, 'swap_model', 'inswapper_128')}; "
                              f"swap backend={getattr(gpu, 'swap_execution', 'CUDA')}; "
                              f"swap builder={getattr(getattr(gpu, 'swapper', None), 'builder_level', '-')}; "
                              f"detector={getattr(gpu, 'detector_backend', 'CUDA')}; "
                              f"occlusion={getattr(getattr(gpu, 'occluder', None), 'backend', 'off')}; "
                              f"regions={getattr(getattr(gpu, 'parser', None), 'backend', 'off')}; "
                              f"landmarker={getattr(gpu, 'landmarker_backend', 'off')}; refined={getattr(gpu, 'landmarks_refined', False)}; pose={getattr(getattr(gpu, 'portrait', None), 'pose_backend', '-')}; registered={getattr(getattr(gpu, 'portrait_registration', None), 'applied', False)}; registration_status={getattr(getattr(gpu, 'portrait_registration', None), 'reason', '-')}; registration_error_px={getattr(getattr(gpu, 'portrait_registration', None), 'error', None)}; shape_active={getattr(gpu, 'shape_active', False)}; "
                              f"restoration={getattr(gpu, 'restoration_backend', '?')}; stages wall ms: {stages}", flush=True)
                        last_report = now
                except Exception as error:
                    if gpu:
                        gpu.close()
                    gpu = None
                    gpu_key = None
                    import traceback
                    traceback.print_exc()
                    print(f"GPU-resident path unavailable; CPU pixel fallback: {error}", flush=True)
                    reason = str(error).encode("utf-8")[:4096]
                    connection.sendall(HEADER.pack(GPU_ERROR, width, height, len(reason)) + reason)
                continue
            width, height, size = frame_header(header)
            payload = read_exact(connection, size)
            started = time.perf_counter()
            result = engine.swap(payload, width, height)
            if len(result) != size:
                raise ValueError("Invalid face-swap output size")
            connection.sendall(header + result)
            now = time.perf_counter()
            if now - last_report >= 2:
                stages = ", ".join(f"{name}={duration:.1f}" for name, duration in getattr(engine, "last_timings", {}).items())
                print(f"Frame {width}x{height}: {(now - started) * 1000:.1f} ms; "
                      f"target faces: {getattr(engine, 'last_face_count', '?')}; stages ms: {stages}", flush=True)
                last_report = now
    finally:
        if gpu:
            gpu.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--detector", type=Path, required=True, help="SCRFD detector ONNX with five landmarks")
    parser.add_argument("--detector-kind", choices=("scrfd","yoloface","retinaface","yunet"),default="scrfd")
    parser.add_argument("--recognizer", type=Path, required=True, help="ArcFace recognition ONNX")
    parser.add_argument("--swapper", type=Path, required=True, help="inswapper_128.onnx")
    from face_swap_models import MODEL_NAMES
    parser.add_argument("--swap-model", choices=MODEL_NAMES, default="inswapper_128")
    parser.add_argument("--swapper-backend", choices=("cuda", "tensorrt_fp32", "tensorrt_fp16"), default="cuda")
    parser.add_argument("--swapper-builder", type=int, choices=(0,3,5), default=5, help="TensorRT compilation for all active GPU models: optimized 5 (default), standard 3; legacy 0 means 5")
    parser.add_argument("--backend", choices=("cuda", "directml", "cpu"), default="cuda")
    parser.add_argument("--device", type=int, default=0, help="GPU index (CUDA index or DirectML DXGI adapter index)")
    parser.add_argument("--restorer", type=Path, help="Optional GPEN-BFR-256/512 ONNX")
    parser.add_argument("--restore-strength", type=float, default=.6, help="Restored face blend, 0..1")
    parser.add_argument("--port", type=int, default=7011, help="Loopback port, 0 selects a private available port")
    parser.add_argument("--cache-root", type=Path, help="App-local swapper folder containing prepared, tensorrt-engines and cuda-kernels")
    parser.add_argument("--ready-file", type=Path, help="Write the bound port atomically after model loading")
    parser.add_argument("--single-client", action="store_true", help="Exit after the owning receiver disconnects")
    parser.add_argument("--gpu-bridge", type=Path, help="Optional CUDA/D3D11 driver bridge DLL")
    parser.add_argument("--mask-shape", choices=("ellipse","box"), default="ellipse")
    parser.add_argument("--mask-padding", type=int, nargs=4, default=(0,0,0,0), metavar=("TOP","RIGHT","BOTTOM","LEFT"))
    parser.add_argument("--alignment", choices=("legacy","facefusion"), default="legacy")
    parser.add_argument("--face-size", type=int, choices=range(80,121), default=100)
    parser.add_argument("--face-scale", type=int, choices=range(80,121), default=100)
    parser.add_argument("--animation", action="store_true", help="Animate the reference photo using LivePortrait on the GPU")
    parser.add_argument("--animation-strength", type=int, choices=range(101), default=100)
    parser.add_argument("--shape-transfer", type=int, choices=range(101), default=0, help=argparse.SUPPRESS)
    parser.add_argument("--shape-reset", type=int, default=0, help=argparse.SUPPRESS)
    parser.add_argument("--mask-expansion", type=int, choices=range(13), default=0)
    parser.add_argument("--mask-blur", type=int, default=18)
    parser.add_argument("--mask-preview", type=int, choices=(0,1,2), default=0)
    parser.add_argument("--landmarker", type=Path, help="Optional HRFFA 68-point target refinement, GPU-resident CUDA only")
    parser.add_argument("--face-parser", type=Path, help="Optional BiSeNet ResNet18, GPU-resident CUDA only")
    parser.add_argument("--parser-mode", type=int, choices=(1,2,3,4), default=1)
    parser.add_argument("--occluder", type=Path, help="Optional XSeg 1 occlusion protection, GPU-resident CUDA only")
    parser.add_argument("--tensorrt", action="store_true", help="Optional TensorRT FP32 for GPU-resident GPEN 256/512")
    args = parser.parse_args()
    if args.animation and (args.backend!='cuda' or not args.gpu_bridge or not args.gpu_bridge.is_file()):
        parser.error('LivePortrait requires NVIDIA CUDA and a valid --gpu-bridge')
    if not 0<=args.shape_reset<=2147483647 or any(v<0 or v>45 for v in args.mask_padding) or not 0<=args.mask_blur<=50:
        parser.error("Mask padding must be 0..45 and edge softness 0..50 percent")
    configure(args.ready_file)
    report("Loading face detection and identity models")
    if not 0 <= args.port <= 65535:
        parser.error("Port must be between 0 and 65535")
    if not 0 <= args.restore_strength <= 1:
        parser.error("Restore strength must be between 0 and 1")
    for path in ((args.detector,) if args.animation else (args.detector, args.recognizer, args.swapper)):
        if not path.is_file():
            parser.error(f"Model not found: {path}")
    if args.face_parser and not args.face_parser.is_file():
        parser.error(f"Face parser model not found: {args.face_parser}")
    if args.occluder and not args.occluder.is_file():
        parser.error(f"Occluder model not found: {args.occluder}")
    if args.restorer and not args.restorer.is_file():
        parser.error(f"Restoration model not found: {args.restorer}")
    engine_directory = Path(__file__).resolve().parent
    asset_directory = engine_directory.parent if engine_directory.name == "engine" else engine_directory
    cache_root = (args.cache_root or asset_directory / "swappers").resolve()
    engine = CudaEngine(args.detector, args.recognizer, args.swapper, args.device, args.restorer, args.restore_strength,
                        swap_model=args.swap_model,
                        model_cache=cache_root / "prepared",
                        swapper_backend=args.swapper_backend, backend=args.backend, swapper_builder=args.swapper_builder,detector_kind=args.detector_kind,animation=args.animation)
    gpu_config = None
    if args.backend == "cuda" and args.gpu_bridge and args.gpu_bridge.is_file() and importlib.util.find_spec("cupy"):
        cache = cache_root / "cuda-kernels"
        gpu_config = {"dll": args.gpu_bridge, "cache": cache, "restorer": args.restorer, "strength": args.restore_strength,
                      "tensorrt": args.tensorrt, "prepare": True, "landmarker":args.landmarker, "occluder": args.occluder, "parser":args.face_parser, "parser_mode":args.parser_mode,
                      "mask_config":{"shape":args.mask_shape,"padding":args.mask_padding,"blur":args.mask_blur,"preview":args.mask_preview,"face_scale":args.face_scale,"face_size":args.face_size,"alignment":int(args.alignment=="facefusion"),"expansion":args.mask_expansion,"shape_transfer":args.shape_transfer,"shape_reset":args.shape_reset,"animation_strength":args.animation_strength}}
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        # A single local receiver, no LAN listener or remote photo upload.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        server.bind(("127.0.0.1", args.port))
        server.listen(1)
        port = server.getsockname()[1]
        if args.ready_file:
            temporary = args.ready_file.with_name(args.ready_file.name + ".tmp")
            temporary.write_text(str(port), encoding="ascii")
            temporary.replace(args.ready_file)
        print(f"{args.backend.upper()} face-swap worker ready on 127.0.0.1:{port}", flush=True)
        while True:
            connection, _ = server.accept()
            with connection:
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.settimeout(None if args.single_client else 15)
                try:
                    serve_client(connection, engine, gpu_config)
                except (EOFError, ConnectionError, TimeoutError):
                    pass
                except Exception as error:
                    print(f"Face swap failed: {error}", file=sys.stderr, flush=True)
                finally:
                    engine.source = None
            if args.single_client:
                return


if __name__ == "__main__":
    main()
