"""Compare CUDA Graphs on the resident image path using GPU-held photo fixtures.

Includes GPU fixture reset, detection, swap, optional GPEN and blending.
Excludes D3D11 interop, sockets, AirPlay and presentation. Host image transfers
are confined to fixture setup and output verification outside measurements.
"""
import argparse
import gc
import json
import os
from pathlib import Path
import statistics
from unittest.mock import patch

from face_swap_worker import CudaEngine
import face_swap_gpu_worker as gpu


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--quality", choices=["fast", "balanced", "detail"], default="balanced")
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--comparison", choices=["graphs", "tensorrt"], default="graphs")
    parser.add_argument("--cache-dir", type=Path, default=Path("build/cupy-cache"))
    args = parser.parse_args()
    width, height = map(int, args.size.lower().split("x"))
    if args.frames < 4 or not (2 <= width <= 1920 and 2 <= height <= 1920) or width % 2 or height % 2:
        parser.error("Use at least 4 frames and even dimensions in 2..1920")
    root = args.models
    swapper = root / "inswapper_128.onnx"
    if not swapper.is_file():
        swapper = root / "inswapper_128.fp16.onnx"
    base = CudaEngine(root / "det_10g.onnx", root / "w600k_r50.onnx", swapper, 0, swap_graph=False)
    base.set_source(args.source)
    np, cv2 = base.np, base.cv2
    photo = cv2.imdecode(np.fromfile(str(args.source), dtype=np.uint8), cv2.IMREAD_COLOR)
    ratio = min(width / photo.shape[1], height / photo.shape[0])
    scaled = cv2.resize(photo, (max(1, int(photo.shape[1]*ratio)), max(1, int(photo.shape[0]*ratio))))
    image = np.zeros((height, width, 3), dtype=np.uint8)
    x, y = (width-scaled.shape[1])//2, (height-scaled.shape[0])//2
    image[y:y+scaled.shape[0], x:x+scaled.shape[1]] = scaled
    shifted = cv2.warpAffine(image, np.float32([[1, 0, 8], [0, 1, 4]]), (width, height))
    cp = gpu.load_cupy(args.cache_dir.resolve())
    fixtures = [cp.asarray(cv2.cvtColor(value, cv2.COLOR_BGR2BGRA)) for value in (image, shifted)]
    cp.cuda.get_current_stream().synchronize()

    class FixtureBridge:
        def __init__(self, *unused):
            self.device, self.index = 0, 0

        def read(self, array, stream):
            array[...] = fixtures[self.index % 2]
            self.index += 1

        def write(self, array, stream):
            stream.synchronize()

        def close(self):
            pass

    side = {"fast": 0, "balanced": 256, "detail": 512}[args.quality]
    restorer = root / f"GPEN-BFR-{side}.onnx" if side else None
    results = {"fixture": "two alternating GPU-held still photos", "width": width,
               "height": height, "quality": args.quality, "frames": args.frames, "comparison": args.comparison}
    outputs = []
    for enabled in (False, True):
        capture = enabled or args.comparison == "tensorrt"
        with patch.object(gpu, "TextureBridge", FixtureBridge), patch.dict(os.environ, {"AIRPLAY_FACE_SWAP_CUDA_GRAPHS": str(int(capture))}):
            engine = gpu.GpuImageEngine(base, None, (0, 0, 0), width, height,
                                        args.cache_dir.resolve(), restorer, tensorrt=enabled and args.comparison == "tensorrt")
        if enabled and args.comparison == "tensorrt" and engine.restoration_backend != "TensorRT FP32":
            raise RuntimeError("TensorRT comparison did not activate TensorRT restoration")
        timings, stages, slow_frames = [], {}, []
        for index in range(args.frames + 6):
            elapsed = engine.process()
            if engine.last_face_count < 1:
                raise RuntimeError("Benchmark fixture has no detected face")
            if index >= 6:
                timings.append(elapsed)
                for name, value in engine.last_timings.items():
                    stages.setdefault(name, []).append(value)
                if elapsed >= 100:
                    slow_frames.append({"frame": index-6, "elapsed_ms": round(elapsed, 3),
                        "stages_ms": {key: round(value, 3) for key, value in engine.last_timings.items()}})
                    print("Slow frame", slow_frames[-1], flush=True)
        # Compare both changing inputs after capture: catches stale graph inputs.
        images = []
        for index in range(2):
            engine.bridge.index = index
            engine.process()
            images.append(cp.asnumpy(engine.image))
        outputs.append(images)
        name = args.comparison if enabled else "reference"
        models = {name: model.graph_ready for name, model in
                  (("detector", engine.detector), ("swapper", engine.swapper), ("restorer", engine.restorer)) if model}
        results[name] = {"median_ms": round(statistics.median(timings), 3),
            "p95_ms": round(float(np.percentile(timings, 95)), 3), "max_ms": round(max(timings), 3),
            "slow_frames": slow_frames,
            "stage_medians_ms": {key: round(statistics.median(values), 3) for key, values in stages.items()},
            "captured": models, "restoration_backend": engine.restoration_backend}
        print(name, results[name], flush=True)
        engine.close()
        del engine
        gc.collect()
    delta = np.abs(np.stack(outputs[0]).astype(np.float32)-np.stack(outputs[1]).astype(np.float32))
    results["pixel_difference"] = {"max": int(delta.max()), "mean": round(float(delta.mean()), 6),
                                    "p99": float(np.percentile(delta, 99))}
    results["speedup"] = round(results["reference"]["median_ms"]/results[args.comparison]["median_ms"], 3)
    if np.array_equal(outputs[1][0], outputs[1][1]):
        raise RuntimeError("Graph output did not change with the input fixture")
    print(json.dumps(results, indent=2), flush=True)
    if args.json:
        args.json.write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
