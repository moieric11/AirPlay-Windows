"""Compare TensorRT builder levels 3/5 without changing model precision.

Paired alternating runs use one GPU pipeline, one detector and identical image
fixtures. Each builder level has a separate versioned engine cache. Compilation
is excluded from timings; full-frame readbacks are outside measurements.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import time
from unittest.mock import patch

from face_swap_worker import CudaEngine
from face_swap_models import identity_tensor
from face_swap_gpu_worker import GpuImageEngine, load_cupy
from face_swap_tensorrt import TensorRTModel


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--target", type=Path, required=True)
    p.add_argument("--swappers", type=Path, default=Path("build/Release/face_swap/swappers"))
    p.add_argument("--model", choices=["hyperswap_1a_256", "hyperswap_1b_256", "hyperswap_1c_256"], default="hyperswap_1b_256")
    p.add_argument("--frames", type=int, default=120)
    p.add_argument("--output", type=Path, default=Path("build/tensorrt-builder"))
    args = p.parse_args()
    if args.frames < 12:
        p.error("Use at least 12 paired measured frames")
    args.output.mkdir(parents=True, exist_ok=True)
    path = (args.swappers / (args.model+".onnx")).resolve()
    r = args.models
    base = CudaEngine(r/"det_10g.onnx", r/"w600k_r50.onnx", path, 0,
                      swap_model=args.model, swapper_backend="tensorrt_fp32")
    base.set_source(args.source)
    np, cv2 = base.np, base.cv2
    cache = args.swappers.resolve()/"cuda-kernels"
    cp = load_cupy(cache)
    banks = {}
    for size, photo_path in [((498,1080),args.source), ((1280,720),args.target)]:
        w, h = size
        photo = cv2.imdecode(np.fromfile(str(photo_path), dtype=np.uint8), cv2.IMREAD_COLOR)
        if photo is None:
            raise ValueError("Unreadable fixture: "+str(photo_path))
        ratio = min(w/photo.shape[1], h/photo.shape[0])
        resized = cv2.resize(photo, (int(photo.shape[1]*ratio), int(photo.shape[0]*ratio)))
        image = np.zeros((h,w,3), np.uint8)
        x, y = (w-resized.shape[1])//2, (h-resized.shape[0])//2
        image[y:y+resized.shape[0], x:x+resized.shape[1]] = resized
        moved = cv2.warpAffine(image, np.float32([[1,0,8],[0,1,4]]), (w,h))
        banks[size] = [cp.asarray(cv2.cvtColor(v, cv2.COLOR_BGR2BGRA)) for v in (image,moved,np.zeros_like(image))]
    cp.cuda.get_current_stream().synchronize()

    class Bridge:
        def __init__(self, dll, ticket, w, h):
            self.device, self.size, self.index = 0, (w,h), 0
        def read(self, image, stream):
            image[...] = banks[self.size][self.index]
        def write(self, image, stream):
            stream.synchronize()
        def close(self):
            pass

    results = {"model":args.model, "fp16_enable":False, "frames_each":args.frames,
               "scope":"GPU-held fixtures, excludes AirPlay, D3D11 and presentation", "cases":{}}
    def reference_swapper(cp, base, stream, device, cache):
        runner=TensorRTModel(cp,path,stream,device,cache.parent/"tensorrt-engines",fp16=False,builder_level=3)
        runner.run()
        return runner,"TensorRT FP32"

    with patch("face_swap_gpu_worker.TextureBridge", Bridge), patch("face_swap_gpu_worker.make_swapper", reference_swapper):
        engine = GpuImageEngine(base,None,(0,0,0),498,1080,cache)
        if engine.swap_execution != "TensorRT FP32":
            raise RuntimeError("Reference swapper did not activate TensorRT")
        runners = {3:engine.swapper}
        print("Building/loading level 5; precision/workspace/streams unchanged...", flush=True)
        began = time.perf_counter()
        with engine.stream:
            runners[5] = TensorRTModel(cp,path,engine.stream,engine.device,cache.parent/"tensorrt-engines",fp16=False,builder_level=5)
            identity = cp.asarray(identity_tensor(args.model,base.source), dtype=cp.float32)
            for runner in runners.values():
                index = next(i for i,v in enumerate(runner.inputs) if v.ndim==2)
                runner.inputs[index][...] = identity
                runner.run()
        engine.stream.synchronize()
        results["level5_load_or_build_seconds"] = time.perf_counter()-began
        results["caches"] = {str(k):str(v.cache) for k,v in runners.items()}
        print("Level 5 ready after", round(results["level5_load_or_build_seconds"],2), "seconds", flush=True)

        for size in banks:
            w,h = size
            if (engine.width,engine.height)!=size:
                assert engine.replace_textures((0,0,0),w,h)
            checks = []
            for fixture in (0,1,2,0):
                images, faces, raw = [], [], []
                for level in (3,5):
                    engine.swapper = runners[level]
                    engine.bridge.index = fixture
                    engine.process()
                    images.append(cp.asnumpy(engine.image))
                    faces.append(engine.last_face_count)
                    raw.append(cp.asnumpy(engine.swapper.outputs[0]))
                    if not np.isfinite(raw[-1]).all(): raise ValueError("Invalid model output")
                assert faces[0]==faces[1]
                delta = np.abs(images[0].astype(np.float32)-images[1].astype(np.float32))
                raw_delta=np.abs(raw[0]-raw[1])*127.5
                if delta.max()>2 or delta.mean()>.01 or (faces[0] and (raw_delta.max()>4 or raw_delta.mean()>.1)):
                    raise ValueError("Builder levels exceed the fixed-fixture image tolerance")
                checks.append({"raw_max_byte_delta":float(raw_delta.max()),"raw_mean_byte_delta":float(raw_delta.mean()),"fixture":fixture,"faces":faces[0],"max_pixel_delta":float(delta.max()),
                               "mean_pixel_delta":float(delta.mean()),"p99_pixel_delta":float(np.quantile(delta,.99))})
                for i,level in enumerate((3,5)):
                    cv2.imwrite(str(args.output/(f"{w}x{h}-{fixture}-level{level}.png")),images[i])
                if not np.isfinite(delta).all():
                    raise ValueError("Invalid engine pixels")
            # Warm each path before paired measurement; alternate order to
            # reduce clock/temperature/order bias between the two engines.
            for _ in range(8):
                for level in (3,5):
                    engine.swapper=runners[level]; engine.bridge.index=0; engine.process()
            timings = {3:[],5:[]}
            swaps = {3:[],5:[]}
            for frame in range(args.frames):
                for level in ((3,5) if frame%2==0 else (5,3)):
                    engine.swapper=runners[level]; engine.bridge.index=frame%2
                    timings[level].append(engine.process())
                    swaps[level].append(engine.last_timings["swap"])
            row = {"quality":checks}
            for level in (3,5):
                row[str(level)] = {"frame_median_ms":statistics.median(timings[level]),
                    "frame_p95_ms":float(np.quantile(timings[level],.95)),
                    "swap_median_ms":statistics.median(swaps[level])}
            row["speedup"] = row["3"]["frame_median_ms"]/row["5"]["frame_median_ms"]
            results["cases"][f"{w}x{h}"] = row
            print(json.dumps(row), flush=True)
            (args.output/"results.json").write_text(json.dumps(results,indent=2),encoding="utf-8")
        engine.close()


if __name__=="__main__":
    main()
