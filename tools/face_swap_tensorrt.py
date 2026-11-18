"""Optional TensorRT device model; fixed GPU IO and versioned local engine caches."""
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import sys

from face_swap_gpu_worker import DeviceModel


def load_tensorrt_libraries():
    """Keep Windows DLL directories alive; no global machine PATH changes."""
    root = Path(sys.prefix) / "Lib" / "site-packages" / "tensorrt_libs"
    if not root.is_dir():
        raise RuntimeError("Install compatible tensorrt-cu12-libs into this Python environment")
    handles = []
    if os.name == "nt":
        os.environ["PATH"] = str(root) + os.pathsep + os.environ.get("PATH", "")
        handles.append(os.add_dll_directory(str(root)))
    return handles


class TensorRTModel(DeviceModel):
    def __init__(self, cp, path, stream, device, cache, fp16=False, profile=False, builder_level=3):
        if type(builder_level) is not int or not 0 <= builder_level <= 5:
            raise ValueError("TensorRT builder level must be an integer in 0..5")
        import onnxruntime as ort
        properties = cp.cuda.runtime.getDeviceProperties(device)
        name = properties["name"]
        if isinstance(name, bytes):
            name = name.decode()
        fingerprint = {"model": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            "ort": ort.__version__, "trt": importlib.metadata.version("tensorrt-cu12-libs"),
            "gpu": name, "compute": [properties["major"], properties["minor"]],
            "driver": cp.cuda.runtime.driverGetVersion(), "fp16": fp16, "builder": builder_level}
        key = hashlib.sha256(json.dumps(fingerprint, sort_keys=True).encode()).hexdigest()[:24]
        self.cache = Path(cache).resolve() / (Path(path).stem + "-" + key)
        self.cache.mkdir(parents=True, exist_ok=True)
        (self.cache / "manifest.json").write_text(json.dumps(fingerprint, indent=2), encoding="utf-8")
        self.fp16, self.profile, self.builder_level = fp16, profile, builder_level
        self.dll_handles = load_tensorrt_libraries()
        super().__init__(cp, path, stream, device, cuda_graph=False)

    def create_session(self):
        options = self.ort.SessionOptions()
        options.log_severity_level = 3
        options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
        if self.profile:
            options.enable_profiling = True
            options.profile_file_prefix = str(self.cache / "profile")
        from face_swap_progress import report
        cached = any(self.cache.glob("*.engine"))
        report(f"TensorRT: loading cache / checking engine - {Path(self.path).stem} (level {self.builder_level})" if cached else
               f"TensorRT: building engine - {Path(self.path).stem} (level {self.builder_level})")
        session = self.ort.InferenceSession(self.path, sess_options=options, providers=[
            ("TensorrtExecutionProvider", {"device_id": self.device,
                "user_compute_stream": str(self.stream.ptr), "trt_fp16_enable": self.fp16,
                "trt_engine_cache_enable": True, "trt_engine_cache_path": str(self.cache),
                "trt_timing_cache_enable": True, "trt_timing_cache_path": str(self.cache),
                "trt_max_workspace_size": 2 * 1024**3, "trt_builder_optimization_level": self.builder_level,
                "trt_cuda_graph_enable": True, "trt_auxiliary_streams": 0}),
            ("CUDAExecutionProvider", {"device_id": self.device, "user_compute_stream": str(self.stream.ptr)})])
        session.disable_fallback()
        if "TensorrtExecutionProvider" not in session.get_providers():
            raise RuntimeError("TensorRT failed to load; refusing a misleading CUDA-only benchmark")
        return session
