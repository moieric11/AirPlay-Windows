# Experimental swapper comparison

The preview now offers InSwapper 128, AlphaFace 256 and HyperSwap 1A/1B/1C 256.
InSwapper remains the default, with the existing Fast / Balanced / Detail modes.
The experimental 256 models generate their own face output without GPEN. Their
full-frame video limit is 1920 pixels on the longer side; 256 refers only to the
aligned face crop. Disabling TensorRT keeps the selected model on CUDA Graphs.

Open **Faceswap Settings** above **Settings** in the navigation. To compare,
disable face swap, select **Swapper model**, keep GPU decoding and
**Keep video on GPU** enabled, then enable the effect. For the 256 models,
**TensorRT swapper** selects FP32 TensorRT; it does not enable
GPEN. Model choice and the TensorRT preference are saved. The existing InSwapper
quality preference is retained when switching back.

Here the TensorRT "FP32" label means its additional FP16 optimization option is
disabled. HyperSwap's published ONNX already contains FP16 initializers; those
are retained as released. AlphaFace's initializers are FP32. No extra precision
conversion of either model is performed.

## Provisioning

The normal build deploys adapters, not pretrained model weights. The local test
installation has been provisioned under `build/Release/face_swap/swappers`.
Other installations can fetch the pinned official FaceFusion release assets:

```powershell
.\.venv-face-swap\Scripts\python.exe tools/download_face_swap_models.py
.\.venv-face-swap\Scripts\python.exe tools/download_face_swap_models.py --verify-only
```

`--models alphaface_256 hyperswap_1c_256` limits the download. `--directory`
selects another receiver's `face_swap/swappers` folder. Each download is verified
against the SHA-256 manifest before replacing its destination. Publisher CRC32
values were also checked during local provisioning. Existing model weights in
the selected detector/recognition models folder are never modified.

Pinned sources:

- AlphaFace: FaceFusion assets `models-3.9.0/alphaface_256.onnx`.
- HyperSwap A/B/C: FaceFusion assets `models-3.3.0/hyperswap_1[a-c]_256.onnx`.
- [Published model IO conventions and release links](https://github.com/facefusion/facefusion/blob/master/facefusion/processors/modules/face_swapper/core.py).
- [AlphaFace authors and pretrained demo](https://github.com/andrewyu90/Alphaface_Official).
- [HyperSwap project](https://github.com/facefusion/facefusion-labs/tree/master/hyperswap).

Follow the licenses of the selected weights; these optional assets are not
relicensed by the receiver. FaceFusion describes AlphaFace as non-commercial
and HyperSwap as ResearchRAIL. No FaceFusion application code was copied into
the adapters; the model interfaces informed an independent implementation.

## Implementation

InSwapper uses its projected, normalized ArcFace identity vector. AlphaFace
expects the raw ArcFace embedding, while HyperSwap expects the normalized
embedding without InSwapper's projection. Both 256 families use the ArcFace 128
alignment template scaled to their crop size. AlphaFace accepts RGB in 0..1;
HyperSwap uses -1..1 for input and output. Input roles are selected by shape,
because HyperSwap places its identity input before its image input.

The AlphaFace export has dynamic batch annotations and incorrectly aliases an
output spatial dimension to batch. The adapter specializes batch to one, repairs
the output declaration and runs symbolic shape inference into a separate cache
file. Original weights remain unchanged. Cache writes use unique temporary files
and atomic replacement. This allows strict CUDA-only node assignment and fixed
device IO Binding. CPU pixel transport also has an adapter for the selected model;
loss of texture interop never silently changes it back to InSwapper.

Both CUDA Graphs and optional TensorRT FP32 use persistent GPU input/output
buffers. TensorRT uses the existing versioned engine cache and user compute
stream. A TensorRT initialization failure retains the selected weights on CUDA.
The UI retains cancellation during first-time builds. Logs report the selected
swapper and actual execution backend. FP16 TensorRT is available only through
the diagnostic worker/benchmark argument; it is not exposed by the application.

## Initial measurements and their limits

One supplied source photo was compared against a different, isolated target
face from InsightFace's public demonstration image. Two translated target stills
alternate at 1280x720. The target was isolated before scoring, because selecting
the largest person again after swapping could score a different face. Source and
target embeddings were distinct (cosine -0.004). This is not a live-video or
multi-person quality evaluation.

RTX 3080 Laptop GPU, six warm-ups; CUDA runs use 40 measured frames and TensorRT
runs use 60. GPU wall-clock timings include detection, swap and optional GPEN,
but exclude D3D11, sockets, AirPlay and presentation. GPU power/clock state and
workload affect these figures; different fixtures should not be compared as
if they had identical latency.

| Pipeline | Median | P95 |
|---|---:|---:|
| InSwapper Fast, CUDA Graphs | 16.867 ms | 18.111 ms |
| InSwapper + GPEN 256 TensorRT | 40.893 ms | 42.119 ms |
| AlphaFace 256, CUDA Graphs, no GPEN | 82.506 ms | 104.689 ms |
| AlphaFace 256, TensorRT FP32, no GPEN | 72.382 ms | 99.976 ms |
| HyperSwap 1A, CUDA Graphs, no GPEN | 25.459 ms | 27.135 ms |
| HyperSwap 1A, TensorRT FP32, no GPEN | 19.636 ms | 21.046 ms |
| HyperSwap 1B, CUDA Graphs, no GPEN | 25.454 ms | 27.483 ms |
| HyperSwap 1B, TensorRT FP32, no GPEN | 19.595 ms | 21.098 ms |
| HyperSwap 1C, CUDA Graphs, no GPEN | 25.327 ms | 26.983 ms |
| HyperSwap 1C, TensorRT FP32, no GPEN | 19.905 ms | 21.703 ms |

TensorRT versus CUDA output differences on the same target were maximum one
level for AlphaFace and two for HyperSwap, with full-frame mean differences
0.003918 and 0.007935..0.008710 on a 0-255 scale. These compare execution
backends, not visual fidelity against the source person.

Recognition cosine scores were 0.874 for InSwapper + GPEN, 0.832 for AlphaFace,
and 0.793 / 0.829 / 0.835 for HyperSwap A/B/C under TensorRT. The same recognition
encoder participates in the swap, so these scores are diagnostics, not independent
quality rankings. On this fixture the current restored pipeline looked sharper;
HyperSwap B/C offer useful alternatives to assess on live motion. AlphaFace was
slower and did not establish an overall improvement. No default was changed.

All four new models passed the managed D3D11 -> CUDA/ONNX/TensorRT -> D3D11
photo test and a resolution change without recreating model sessions. Twenty
automated tests cover protocol/lifecycle, persistent buffers, restoration fallback,
identity conventions, normalization, selected-model fallback, shape-cache reuse
and source-model immutability. Settings checks cover model/backend persistence.

## Reproduction

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_swapper_models.py `
  --models "D:\models" --swappers build/Release/face_swap/swappers `
  --source "D:\photos\source.jpg" --target "D:\photos\different-target.jpg" `
  --backend cuda --frames 40 --output build/swapper-comparison

.\.venv-face-swap\Scripts\python.exe tools/benchmark_swapper_models.py `
  --models "D:\models" --swappers build/Release/face_swap/swappers `
  --source "D:\photos\source.jpg" --target "D:\photos\different-target.jpg" `
  --select alphaface_256 hyperswap_1a_256 hyperswap_1b_256 hyperswap_1c_256 `
  --backend tensorrt_fp32 --frames 60 --output build/swapper-comparison-trt
```

Outputs include JSON timings, diagnostic recognition scores and private local
comparison images. Uploads are not part of the worker or benchmark. The local
test used the previously captured source pixels after the NAS rejected access;
no network credentials were changed. Live temporal quality, occlusions and a
wider variety of identities remain for actual AirPlay testing.

The in-app Download / Verify preview supports the four native 256 swappers.
The download manifest pins Hugging Face revisions, file sizes and SHA-256 values;
license metadata is displayed before the user initiates a download. Existing
valid files are reused; incomplete or corrupt transfers do not replace them.
