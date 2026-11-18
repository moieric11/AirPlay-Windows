"""Benchmark GPEN CUDA Graphs versus TensorRT on changing GPU-held face crops.

Model weights and photos are read-only. Engine caches, JSON and optional visual
comparisons are written under --output. Timings exclude fixture upload, source
analysis, AirPlay and D3D11 transport. TensorRT is required, never silently CPU.
"""
import argparse
import gc
import json
from pathlib import Path
import statistics
import time

from face_swap_worker import CudaEngine, largest_face
from face_swap_gpu_worker import DeviceModel, load_cupy
from face_swap_tensorrt import TensorRTModel



def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--side", type=int, choices=[256, 512], default=256)
    p.add_argument("--frames", type=int, default=60)
    p.add_argument("--output", type=Path, default=Path("build/tensorrt-face"))
    p.add_argument("--modes", nargs="+", choices=["cuda", "trt_fp32", "trt_fp16"],
                   default=["cuda", "trt_fp32", "trt_fp16"])
    args = p.parse_args()
    if args.frames < 4:
        p.error("Use at least four frames")
    args.output.mkdir(parents=True, exist_ok=True)
    r = args.models
    swap = r / "inswapper_128.onnx"
    if not swap.is_file():
        swap = r / "inswapper_128.fp16.onnx"
    base = CudaEngine(r / "det_10g.onnx", r / "w600k_r50.onnx", swap, 0)
    base.set_source(args.source)
    np, cv2 = base.np, base.cv2
    photo = cv2.imdecode(np.fromfile(str(args.source), dtype=np.uint8), cv2.IMREAD_COLOR)
    face = largest_face(base.faces(photo))
    if face is None:
        raise RuntimeError("Fixture photo has no target face")
    swapped = base.swap_face(photo.copy(), face)
    from insightface.utils.face_align import norm_crop2
    crop, _ = norm_crop2(swapped, face.kps, args.side)
    crops = [crop, np.clip(crop.astype(np.int16)+3, 0, 255).astype(np.uint8)]
    cp = load_cupy(args.output / "cupy-cache")
    fixtures = [cp.asarray(np.ascontiguousarray((c[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32)-127.5)/127.5))
                for c in crops]
    cp.cuda.get_current_stream().synchronize()
    stream = cp.cuda.Stream(non_blocking=True)
    results = {"fixture": "two alternating aligned face crops", "side": args.side,
               "frames": args.frames, "timings_exclude": "upload, alignment, blending, D3D11, AirPlay"}
    outputs = {}
    for mode in args.modes:
        started = time.perf_counter()
        print(f"Building/loading {mode} GPEN {args.side}...", flush=True)
        with stream:
            runner = (DeviceModel(cp, r / f"GPEN-BFR-{args.side}.onnx", stream, 0, cuda_graph=True)
                if mode == "cuda" else TensorRTModel(cp, r / f"GPEN-BFR-{args.side}.onnx", stream, 0,
                    args.output / "engines", fp16=mode == "trt_fp16"))
            runner.inputs[0][...] = fixtures[0]
            runner.run()
        stream.synchronize()
        build_seconds = time.perf_counter()-started
        timings = []
        with stream:
            for index in range(args.frames+6):
                started = time.perf_counter()
                runner.inputs[0][...] = fixtures[index % 2]
                runner.run()
                stream.synchronize()
                if index >= 6:
                    timings.append((time.perf_counter()-started)*1000)
            images = []
            for fixture in fixtures:
                runner.inputs[0][...] = fixture
                raw = runner.run()[0]
                stream.synchronize()
                raw = cp.asnumpy(raw)
                if not np.isfinite(raw).all():
                    raise RuntimeError(f"{mode} returned nonfinite output")
                images.append(np.clip((raw[0].transpose(1, 2, 0)+1)*127.5, 0, 255)[:, :, ::-1].astype(np.uint8))
        outputs[mode] = images
        results[mode] = {"median_ms": round(statistics.median(timings), 3),
            "p95_ms": round(float(np.percentile(timings, 95)), 3), "max_ms": round(max(timings), 3),
            "build_load_seconds": round(build_seconds, 2), "providers": runner.session.get_providers()}
        cv2.imwrite(str(args.output / f"{mode}.png"), np.concatenate(images, axis=1))
        print(mode, results[mode], flush=True)
        del runner
        gc.collect()
        (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if "cuda" in outputs:
        for mode in args.modes:
            delta = np.abs(np.stack(outputs[mode]).astype(np.float32)-np.stack(outputs["cuda"]).astype(np.float32))
            results[mode]["pixel_difference"] = {"mean": round(float(delta.mean()), 5),
                "max": int(delta.max()), "p99": float(np.percentile(delta, 99))}
        cv2.imwrite(str(args.output / "comparison.png"), np.concatenate([outputs[mode][0] for mode in args.modes], axis=1))
    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results, indent=2), flush=True)


if __name__ == "__main__":
    main()
