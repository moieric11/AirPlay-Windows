"""Local comparison of native 256 swappers and InSwapper + GPEN.

Different source/target identities; two alternating target images. GPU timing
excludes D3D11, sockets and display. Output readbacks are test-only. Recognition
similarity is diagnostic, not a perceptual-quality or identity guarantee.
"""
import argparse
import gc
import json
from pathlib import Path
import statistics
from unittest.mock import patch

from face_swap_worker import CudaEngine, largest_face
import face_swap_gpu_worker as gpu


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--swappers", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--target", type=Path, required=True)
    p.add_argument("--frames", type=int, default=40)
    p.add_argument("--output", type=Path, default=Path("build/swapper-comparison"))
    p.add_argument("--backend", choices=("cuda", "tensorrt_fp32", "tensorrt_fp16"), default="cuda")
    p.add_argument("--select", nargs="+", default=["inswapper_fast", "inswapper_balanced", "alphaface_256",
                   "hyperswap_1a_256", "hyperswap_1b_256", "hyperswap_1c_256"])
    args = p.parse_args()
    if args.frames < 4:
        p.error("Use at least four measured frames")
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.source.is_file() or not args.target.is_file():
        p.error("Source and target fixtures must be readable local files")
    r = args.models
    inswapper = r / "inswapper_128.onnx"
    if not inswapper.is_file():
        inswapper = r / "inswapper_128.fp16.onnx"
    results, rendered = {}, []
    for selected in args.select:
        engine = base = None
        try:
            native = not selected.startswith("inswapper_")
            path = args.swappers / (selected+".onnx") if native else inswapper
            base = CudaEngine(r / "det_10g.onnx", r / "w600k_r50.onnx", path, 0,
                              swap_model=selected if native else "inswapper_128",
                              model_cache=Path("build/swapper-comparison/prepared"),
                              swapper_backend=args.backend if native else "cuda")
            base.set_source(args.source)
            np, cv2 = base.np, base.cv2
            target = cv2.imdecode(np.fromfile(str(args.target), dtype=np.uint8), cv2.IMREAD_COLOR)
            original_face = largest_face(base.faces(target))
            if original_face is None:
                raise RuntimeError("Original target has no detected face")
            # Isolate one target: otherwise the largest-face selector may
            # choose a different person after the swap and corrupt ID scores.
            box = original_face.bbox.astype(int)
            pad = int(max(box[2]-box[0], box[3]-box[1])*.5)
            target = target[max(0,box[1]-pad):min(target.shape[0],box[3]+pad),
                            max(0,box[0]-pad):min(target.shape[1],box[2]+pad)]
            ratio = min(1280/target.shape[1], 720/target.shape[0])
            resized = cv2.resize(target, (int(target.shape[1]*ratio), int(target.shape[0]*ratio)))
            image = np.zeros((720, 1280, 3), dtype=np.uint8)
            y, x = (720-resized.shape[0])//2, (1280-resized.shape[1])//2
            image[y:y+resized.shape[0], x:x+resized.shape[1]] = resized
            target_face = largest_face(base.faces(image))
            if target_face is None:
                raise RuntimeError("Target fixture has no face")
            base.recognizer.get(image, target_face)
            source_target_cosine = float(np.dot(target_face.normed_embedding, base.source.normed_embedding))
            shifted = cv2.warpAffine(image, np.float32([[1, 0, 8], [0, 1, 4]]), (1280, 720))
            cp = gpu.load_cupy(Path("build/cupy-cache").resolve())
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

            restorer = r / "GPEN-BFR-256.onnx" if selected == "inswapper_balanced" else None
            with patch.object(gpu, "TextureBridge", FixtureBridge):
                engine = gpu.GpuImageEngine(base, None, (0, 0, 0), 1280, 720,
                                            Path("build/cupy-cache").resolve(), restorer, tensorrt=True)
            times, stages = [], {}
            if native and args.backend != "cuda" and not engine.swap_execution.startswith("TensorRT"):
                raise RuntimeError("Requested TensorRT benchmark fell back to CUDA")
            for index in range(args.frames+6):
                elapsed = engine.process()
                if not engine.last_face_count:
                    raise RuntimeError("GPU detector lost the fixture face")
                if index >= 6:
                    times.append(elapsed)
                    for name, value in engine.last_timings.items():
                        stages.setdefault(name, []).append(value)
            engine.bridge.index = 0
            engine.process()
            output = cp.asnumpy(engine.image)[:, :, :3]
            detected = base.Face(kps=target_face.kps.copy())
            base.recognizer.get(output, detected)
            similarity = float(np.dot(detected.normed_embedding, base.source.normed_embedding))
            cv2.imwrite(str(args.output / (selected+".png")), output)
            # Keep one identical target crop for visual comparison.
            box = target_face.bbox.astype(int)
            pad = max(box[2]-box[0], box[3]-box[1])//4
            crop = output[max(0,box[1]-pad):min(720,box[3]+pad),max(0,box[0]-pad):min(1280,box[2]+pad)]
            crop = cv2.resize(crop, (320, 400))
            cv2.putText(crop, selected, (5, 24), cv2.FONT_HERSHEY_SIMPLEX, .55, (0,255,0), 1)
            rendered.append(crop)
            results[selected] = {"median_ms": round(statistics.median(times), 3),
                "p95_ms": round(float(np.percentile(times,95)), 3),
                "stages_ms": {k: round(statistics.median(v),3) for k,v in stages.items()},
                "source_output_cosine": round(similarity,4), "source_target_cosine": round(source_target_cosine,4),
                "restoration": engine.restoration_backend, "backend": engine.swap_execution}
            print(selected, results[selected], flush=True)
        except Exception as error:
            import traceback
            traceback.print_exc()
            results[selected] = {"error": str(error)}
        finally:
            if engine:
                engine.close()
            engine = base = None
            gc.collect()
            (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if rendered:
        cv2.imwrite(str(args.output / "comparison.png"), np.concatenate(rendered, axis=1))
    if any("error" in result for result in results.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
