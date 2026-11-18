"""Compare original and optimized CUDA paths using local models and a photo.

This is a repeated still-image fixture, not a live AirPlay latency test.
No model downloads or source-image changes are performed.
"""
import argparse
import gc
import json
from pathlib import Path
import statistics
import time

from face_swap_worker import CapturedSwapper, CudaEngine, FaceRestorer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--quality", choices=["fast", "balanced", "detail"], default="balanced")
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    width, height = map(int, args.size.lower().split("x"))
    if args.frames < 3 or not (2 <= width <= 1920 and 2 <= height <= 1920) or width % 2 or height % 2:
        parser.error("Use at least 3 frames and even dimensions in 2..1920")
    root = args.models
    swapper = root / "inswapper_128.onnx"
    if not swapper.is_file():
        swapper = root / "inswapper_128.fp16.onnx"
    engine = CudaEngine(root / "det_10g.onnx", root / "w600k_r50.onnx", swapper,
                        0, roi_swap=False, swap_graph=False)
    engine.set_source(args.source)
    np, cv2 = engine.np, engine.cv2
    photo = cv2.imdecode(np.fromfile(str(args.source), dtype=np.uint8), cv2.IMREAD_COLOR)
    ratio = min(width / photo.shape[1], height / photo.shape[0])
    scaled = cv2.resize(photo, (max(1, int(photo.shape[1] * ratio)), max(1, int(photo.shape[0] * ratio))))
    image = np.zeros((height, width, 3), dtype=np.uint8)
    y, x = (height - scaled.shape[0]) // 2, (width - scaled.shape[1]) // 2
    image[y:y + scaled.shape[0], x:x + scaled.shape[1]] = scaled
    payload = image.tobytes()
    restore_side = {"fast": 0, "balanced": 256, "detail": 512}[args.quality]
    providers = [("CUDAExecutionProvider", {"device_id": 0}), "CPUExecutionProvider"]
    options = engine.detector.session.get_session_options()
    results = {"fixture": "still photo, letterboxed", "width": width, "height": height,
               "quality": args.quality, "frames": args.frames}
    outputs = []
    for optimized in (False, True):
        if optimized:
            engine.swapper.session = CapturedSwapper(engine.swapper.session, 0)
        engine.roi_swap = optimized
        engine.restorer = FaceRestorer(root / f"GPEN-BFR-{restore_side}.onnx", providers,
            options, .6, cuda_graph=optimized) if restore_side else None
        timings = []
        stages = {"detect": [], "swap": [], "restore": []}
        for index in range(args.frames + 2):
            start = time.perf_counter()
            output = engine.swap(payload, width, height)
            elapsed = (time.perf_counter() - start) * 1000
            if index >= 2:
                timings.append(elapsed)
                for name in stages:
                    stages[name].append(engine.last_timings[name])
        name = "optimized" if optimized else "reference"
        results[name] = {"median_ms": round(statistics.median(timings), 2),
            "p95_ms": round(float(np.percentile(timings, 95)), 2),
            "stage_medians_ms": {key: round(statistics.median(values), 2) for key, values in stages.items()}}
        outputs.append(np.frombuffer(output, dtype=np.uint8).reshape(height, width, 3).copy())
        print(name, results[name], flush=True)
        engine.restorer = None
        gc.collect()
    delta = np.abs(outputs[0].astype(np.float32) - outputs[1].astype(np.float32))
    results["pixel_difference"] = {"mean": round(float(delta.mean()), 5), "max": int(delta.max()),
                                   "p99": float(np.percentile(delta, 99))}
    results["speedup"] = round(results["reference"]["median_ms"] / results["optimized"]["median_ms"], 2)
    print(json.dumps(results, indent=2), flush=True)
    if args.json:
        args.json.write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
