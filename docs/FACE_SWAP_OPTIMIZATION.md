# Face-swap optimization notes

The [Deep-Live-Cam swapper](https://github.com/hacksider/Deep-Live-Cam/blob/main/modules/processors/frame/face_swapper.py)
provides relevant comparisons: fixed-shape CUDA Graph replay, persistent GPU
input buffers, cached blending masks and paste-back restricted to the face.
Its [enhancer](https://github.com/hacksider/Deep-Live-Cam/blob/main/modules/processors/frame/face_enhancer.py)
also restricts reconstruction to a crop. Our implementation uses these general
techniques independently; it does not import that project's code.

Its [OpenCV helper](https://github.com/hacksider/Deep-Live-Cam/blob/main/modules/gpu_processing.py)
leaves per-operation CUDA transfers disabled by default. Moving each small blur
or resize separately between host and device can cost more than it saves. That
is also why this receiver does not add independent upload/download steps for
each CPU blend operation.

## Implemented

- The swap runs on a padded face region using the existing InsightFace blend,
  keeping whole-frame warps and intermediate arrays out of the hot path.
- InSwapper uses fixed GPU input/output buffers and CUDA Graph replay. GPEN
  uses the same approach with its existing FP32 model.
- Unsupported capture falls back to normal CUDA execution, retaining model
  precision. Automatic CPU-only session fallback remains disabled.
- GPEN's host input tensor is reused. Socket reads fill one preallocated byte
  buffer instead of repeatedly appending received fragments.
- Logs show detection, swap and restoration times separately. Source recognition
  is still performed once; detection is shared by swap and restoration.
- CMake deploys worker changes even when no native source needs relinking.

The queue remains limited to one pending frame and one completed result. No
frame skipping or reuse of old facial landmarks was added, so moving faces
continue to use current detections.

## Measurements and reproduction

One warmed 1920x1080 still-photo fixture, Balanced mode, 100 measured iterations:

| Path | Median | P95 |
|---|---:|---:|
| Reference: full-frame swap, capture disabled | 156.02 ms | 164.83 ms |
| ROI swap and CUDA Graph replay | 100.05 ms | 105.73 ms |

This is a 1.56x throughput improvement for that fixture. Mean absolute output
difference was 0.00921 levels on a 0–255 scale, maximum 3, P99 zero. Small
rounding differences arise from translating crop coordinates and GPU replay.
These figures exclude AirPlay transport, native pixel conversion and display.
They are not live-video FPS or end-to-end latency guarantees. The fixture uses
a still source photo as its target, so it does not validate a new identity swap
or temporal quality during motion.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_face_swap.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --quality balanced --frames 100 --size 1920x1080 `
  --json build/face-swap-benchmark.json
```

Use `--quality fast` or `--quality detail` for the other modes. The source file
and original model weights are never modified. JSON contains measurements,
not image data.

## Tensor Cores and next steps

[ONNX Runtime CUDA](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html)
supports TF32 on suitable NVIDIA hardware and uses it by default. InSwapper
already uses the locally installed FP16 weights. CUDA Graphs reduce launch
overhead; they are distinct from selecting Tensor Core kernels. Confirming
actual Tensor Core utilization requires a GPU profiler.

An experimental GPEN FP16 conversion was benchmarked using copies in `build/`.
It improved the 512-mode still-image test from roughly 177 to 146 ms, but
introduced larger pixel differences (maximum 96 levels). It was not enabled
in the application. The original weights and GPEN FP32 path remain in use.

[TensorRT FP16](https://onnxruntime.ai/docs/execution-providers/TensorRT-ExecutionProvider.html)
with engine caching is a further option, requiring compatible TensorRT runtime
libraries and per-model validation. TensorRT FP32 for GPEN 256 has since been
validated and integrated as an optional resident-path engine, as detailed below.

An experimental GPU-resident preview now implements D3D11/CUDA texture sharing,
device IO Binding, alignment and blending kernels. See the
[setup, behavior and hardware checks](FACE_SWAP.md#gpu-resident-preview-experimental).
The measurements above describe the compatibility path, not this new pipeline.
The resident path still uses device-to-device copies and explicit synchronization;
live AirPlay measurements should guide further work.

## Resident CUDA Graph replay and latency diagnostics

The resident detector, InSwapper and optional GPEN now capture CUDA Graphs on
their existing fixed device allocations and shared compute stream. Initialization
warms each model before video processing. Capture rejection rebuilds only that
model session with ordinary CUDA IO Binding, without changing tensor precision
or enabling CPU EP fallback. A replay failure after successful capture propagates
to the existing pipeline recovery instead of silently accepting invalid output.

GPU requests now send their 40-byte control message in one call; TCP_NODELAY is
enabled at both ends. This removes avoidable TCP buffering of small messages.

Worker logs include wall-clock `read`, `detect`, `swap`, `restore` and `write_wait`
stages. These do not insert extra GPU synchronization: `write_wait` includes
remaining asynchronous work and the output texture handoff. Every frame taking
at least 250 ms is logged, in addition to the usual two-second report interval.
With the receiver's `--log` option, native logs also distinguish input queue
age, D3D11 preparation, engine round trip and output texture copy. These are
processing measurements, not capture-to-display latency.

On the RTX 3080 Laptop GPU, Balanced mode at 1280x720, two alternating photo
fixtures held in GPU memory, six warm-up frames and 60 measured frames:

| Resident path | Median | P95 |
|---|---:|---:|
| CUDA IO Binding without graphs | 41.353 ms | 43.186 ms |
| CUDA IO Binding with graphs | 31.916 ms | 33.397 ms |

This fixture measured 23% less processing time (1.296x throughput). Detection,
swap and restoration medians were 3.592, 11.849 and 15.750 ms with graphs.
Both changing-input outputs matched the reference exactly. Measurements include
GPU fixture reset and image processing, but exclude D3D11 sharing, sockets,
AirPlay and presentation. They do not predict live FPS or eliminate the
previously observed multi-second stalls; those were not reproduced in this run.
The new stage logs are needed to locate such stalls during live use.

Fast mode also improved, but showed variability between runs: an initial
60-frame comparison measured 21.072 -> 17.319 ms median and included one
929.827 ms outlier with graphs (P95 48.246 ms). A subsequent 120-frame run with
slow-frame stage reporting measured 19.065 -> 14.257 ms median, P95
21.298 -> 15.154 ms and maximum 27.103 -> 17.220 ms, with no frames above
100 ms. Both runs had zero output pixel difference. The isolated stall could
not be localized retrospectively because the first benchmark only retained
aggregate stage medians; the benchmark now saves per-stage data for slow frames.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_gpu_face_swap.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --quality balanced --frames 60 --size 1280x720 `
  --json build/gpu-graphs-balanced.json
```

The benchmark compares graph-disabled and graph-enabled sessions and verifies
both changing inputs after capture. `AIRPLAY_FACE_SWAP_CUDA_GRAPHS=0` disables
resident graph capture for diagnostics; it is not needed for normal operation.
Original model weights and source photos remain unchanged. Detector skipping,
tracking and reduced restoration frequency have not been introduced.

## TensorRT FP32 for GPEN 256

TensorRT 10.9.0.34 CUDA 12 libraries were installed only in the isolated worker
environment, using the official NVIDIA Windows wheel. ONNX Runtime remains
pinned to 1.23.2. Models and source photos remain read-only. GPEN uses device IO
Binding on the existing CUDA stream, TensorRT graph support and versioned engine
and timing caches. Strict session configuration prevents CPU EP execution.
An independent profiled run recorded two TensorRT kernel events and no CUDA or
CPU kernel events for GPEN, confirming real TensorRT execution.

Two alternating aligned GPEN 256 crops, six warm-ups and 60 measured runs on
the RTX 3080 Laptop GPU, excluding alignment, blending and transport:

| GPEN engine | Median | P95 | First build/load |
|---|---:|---:|---:|
| CUDA Graphs | 15.374 ms | 17.000 ms | 0.48 s |
| TensorRT FP32 | 5.610 ms | 8.042 ms | 82.97 s |
| TensorRT FP16 (diagnostic only) | 4.500 ms | 7.803 ms | 444.32 s |

FP32's output differed by a maximum of one byte level, mean 0.01777 on a
0-255 scale. The visual comparison showed no perceptible change on these
fixtures. FP16's mean difference was 9.10508, maximum 152, P99 42; it changed
the generated face visibly and is not used in the app.

The complete GPU image-processing fixture at 1280x720, Balanced mode and 60
measured frames, with detection and swap graphs enabled in both cases:

| Restoration engine | Median | P95 | Maximum |
|---|---:|---:|---:|
| CUDA Graphs | 33.232 ms | 35.640 ms | 37.005 ms |
| TensorRT FP32 | 22.064 ms | 24.716 ms | 26.326 ms |

This measured 33.6% less processing time (1.506x throughput), maximum pixel
difference one, mean 0.000566 over two full-frame outputs. No measured frame
exceeded 100 ms. This still-photo fixture excludes D3D11 sharing, sockets,
AirPlay and presentation; it does not establish live FPS or temporal quality.
The managed native D3D11/CUDA/TensorRT pipeline and same-device resolution change
also passed using a real photo texture. GPEN's engine remains loaded on resize.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_tensorrt_face.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --side 256 --frames 60 --modes cuda trt_fp32 --output build/tensorrt-face

.\.venv-face-swap\Scripts\python.exe tools/benchmark_gpu_face_swap.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --quality balanced --frames 60 --size 1280x720 --comparison tensorrt `
  --json build/gpu-tensorrt-balanced.json
```

The first command writes engine caches under its output directory; the second
uses `build/tensorrt-engines`. The application uses the log-directory cache
documented in [setup](FACE_SWAP.md#tensorrt-restoration-optional). Changing a cache
fingerprint creates a separate directory. Runtime or build failures retain the
CUDA Graph restorer. Fast is unchanged; FP16 and reduced restoration
frequency are not enabled. The optional TensorRT UI checkbox is saved, and the
first GPU reply remains cancellable while allowing a cold engine build.

## TensorRT FP32 for GPEN 512 / Detail

The same FP32 engine now supports GPEN 512 in Detail mode. The existing saved
TensorRT preference applies to both restored modes. Device IO Binding, versioned
caches, initialization fallback to CUDA and cancellable first-build waiting are
shared with GPEN 256. No precision conversion or detection skipping was added.

On the RTX 3080 Laptop GPU, two changing aligned 512-pixel face crops, six
warm-ups and 60 measured iterations, excluding alignment, blending and transport:

| GPEN 512 engine | Median | P95 | Maximum | First build/load |
|---|---:|---:|---:|---:|
| CUDA Graphs | 129.857 ms | 328.838 ms | 334.992 ms | 1.44 s |
| TensorRT FP32 | 56.568 ms | 59.995 ms | 63.989 ms | 188.55 s |

TensorRT's output difference was maximum one level and mean 0.01158 on a
0-255 scale. The visual comparison showed no perceptible change on this fixture.
The CUDA run had higher tail latency; these measurements are specific to this
hardware and test run.

For the complete GPU image-processing fixture at 1280x720, Detail, 60 measured
frames and CUDA Graphs on detection and swap in both cases:

| Restoration engine | Median | P95 | Maximum |
|---|---:|---:|---:|
| CUDA Graphs | 136.376 ms | 141.196 ms | 151.275 ms |
| TensorRT FP32 | 67.197 ms | 73.455 ms | 76.652 ms |

This measured 50.7% less processing time (2.029x throughput). Maximum output
difference was one level, mean 0.000384 across two full-frame outputs. Timings
exclude AirPlay, D3D11 texture sharing, sockets and presentation. Detail still
costs more GPU time than Balanced; these fixtures do not establish live FPS or
temporal quality.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_tensorrt_face.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --side 512 --frames 60 --modes cuda trt_fp32 --output build/tensorrt-face-512

.\.venv-face-swap\Scripts\python.exe tools/benchmark_gpu_face_swap.py `
  --models "D:\models" --source "D:\photos\source.jpg" `
  --quality detail --frames 60 --size 1280x720 --comparison tensorrt `
  --json build/gpu-tensorrt-detail.json
```


## Fused detector metadata decoding

The first Nsight-guided optimization preserves SCRFD, confidence threshold,
NMS, largest-face selection, HyperSwap precision and all model weights.
Previously each frame used CuPy dynamic `flatnonzero` per detection scale,
separate gather/arithmetic/concatenation kernels and multiple metadata downloads.
A dedicated CUDA kernel now decodes all scales into fixed anchor slots, followed
by one download into a reusable pinned host array. Scores are overwritten on
every frame, including empty frames. CPU filtering retains the original anchor
order, including equal-score ties; candidates are never truncated.

For the supplied detector this transfers 4200 * 15 float32 values (252 kB) rather
than a variable few hundred bytes. Trading this bounded metadata transfer for
fewer synchronization points improves latency; no video pixels cross the CPU.

On RTX 3080 Laptop, paired alternating original/optimized runs (60 measured
frames per path, HyperSwap A / TensorRT, no GPEN) gave:

| GPU-held fixture | Original detect | Fused detect | Original frame | Fused frame | Throughput ratio |
|---|---:|---:|---:|---:|---:|
| 1280x720, six faces | 4.016 ms | 2.631 ms | 13.964 ms | 12.317 ms | 1.134x |
| 498x1080, one face | 4.228 ms | 2.586 ms | 14.350 ms | 12.918 ms | 1.111x |

Full outputs were pixel-identical, with identical selected landmarks and face
counts, for both alternating images, black/no-face images, recovery after the
empty input, and a texture resolution change. Opt-in synthetic CUDA checks cover
1/2 anchors, 3/5 detection scales, all/no anchors selected, exact threshold,
NaN scores, and buffer reuse. All selected anchors are retained even in the
worst-case fixture.

A second Nsight capture of the paired benchmark confirmed, per detect/select:
24 -> 2 ordinary kernel launches (including image packing),
5 -> 2 runtime stream synchronizations (including ORT's detector completion),
and 4 -> 1 asynchronous runtime copy calls. CUDA graph launches are separate
from these ordinary kernel counts. These results exclude AirPlay transport,
D3D11 sharing and presentation and do not establish live application FPS.

```powershell
$env:FACE_SWAP_GPU_TESTS='1'
.\.venv-face-swap\Scripts\python.exe -m unittest discover -s tools -p test_face_swap_detection.py
.\.venv-face-swap\Scripts\python.exe tools/benchmark_face_swap_detection.py `
  --models D:/models --source D:/photos/source.png --target D:/photos/target.jpg `
  --frames 60 --output build/detection-optimization
```


## SCRFD detector under TensorRT FP32

After fused metadata decoding, SCRFD 320 remains a separate inference cost.
The resident path now uses TensorRT FP32 for this detector whenever TensorRT
is requested for the swapper or restoration. The 320x320 specialized graph is
saved atomically under `model-cache/scrfd-320-<graph SHA256>.onnx`, leaving the
original detector untouched. Engines use the existing model/ORT/TRT/GPU/driver
fingerprint cache. CUDA Graph detection remains the fallback for build or
warmup failure. The internal diagnostic constructor override
`detector_tensorrt=False` permits an unchanged CUDA baseline in benchmarks.

Initial isolated paired measurements, with fused metadata and HyperSwap A /
TensorRT held constant (RTX 3080 Laptop, 60 frames per path):

| GPU-held fixture | CUDA detection | TRT detection | CUDA total | TRT total |
|---|---:|---:|---:|---:|
| 1280x720, six faces | 2.867 ms | 1.897 ms | 12.702 ms | 11.744 ms |
| 498x1080, one face | 2.758 ms | 1.750 ms | 12.636 ms | 11.777 ms |

This is about 7-8% more pipeline throughput in these fixtures. AirPlay,
D3D11 texture handoff and presentation are excluded, so live FPS gains remain
to be measured. Float32 engine optimizations slightly change detector outputs:
selected landmark differences stayed below 0.035 pixels; full-frame differences
were at most one level on 0..255, with mean at most 0.0032 including alpha.
Face counts remained identical on multi-face, moving, black/no-face and recovered
inputs, including a change from landscape to portrait texture dimensions.
No detector resolution reduction or detection skipping is introduced.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_face_swap_detector_tensorrt.py `
  --models D:/models --source D:/photos/source.png --target D:/photos/target.jpg `
  --frames 60 --output build/detector-tensorrt
```


## Native end-to-end cadence diagnostics

Every managed face-swap session now writes a companion `<worker-log>.cadence.csv`
beside its `.log` file. The native processing thread emits one aggregate row
about every two seconds, plus a final partial interval at shutdown. No GPU
synchronizations are inserted and there is no per-frame file IO. This measures
the existing cadence without changing scheduling, queue depth, vsync or model
execution. Restart the application after updating the native executable.

Run screen mirroring with a moving face for about 30 seconds. The first interval
may include model loading/capture; exclude that interval and very short partial
windows when comparing steady state. The CSV contains:

- `decoded_fps`: frames received by the renderer's decoded-frame producer;
  `input_fps`: AVFrames actually consumed by the render loop.
- `submitted_fps`, `started_fps`, `completed_fps`, `taken_fps`: accepted face-swap
  inputs, worker starts, publishable results, results consumed by the renderer.
- `presented_fps`: new swapped textures presented while the preview is visible
  (Screen mirroring or fullscreen/chromeless), not monitor refresh or physical
  scanout FPS. `render_fps` also counts redraws of an unchanged texture.
- `decoder_replaced`, `pending_replaced`, `output_replaced`: frames overwritten
  in the latest-frame slots at each handoff. Such replacement is intentionally
  bounded to keep latency low and does not alone establish a performance bug.
- `expired`, `wrong_size`: results discarded for freshness/resolution;
  `held`: render loops retaining the previous swapped texture.
- `queue_ms`, `prepare_ms`, `roundtrip_ms`, `copy_ms`: worker queue age, native
  D3D11 preparation, Python engine round trip, output snapshot copy, respectively.
- `idle_ms`: worker wait for a pending input; `result_hold_ms`: completed-result
  wait until `take()`. `render_wait_ms` and `present_wait_ms` expose waits in the
  renderer condition variable and SDL_RenderPresent. `origin_age_ms` measures
  received-frame origin to new swapped presentation submission, not camera-to-
  photon latency. Presentation and result-hold maxima accompany the means.

### Completed-result wakeup

Live measurements of HyperSwap B / TensorRT at 498x1080 exposed completed-result
polling delay. With about 60 decoded frames/s, the worker published 58.7/s but
only 51.3/s were taken/presented. With about 31.5 decoded frames/s, it published
31.5/s but only 26.1/s were presented. Completed-result hold time averaged 7.7 ms
and 20.6 ms in these respective windows. SDL_RenderPresent averaged below 1 ms.
The first loading interval and no-face/low-compute intervals were excluded.

Previously, the renderer only called `take()` when consuming a new AVFrame.
A finished result arriving just after that check waited for the next decoded
input, and could be overwritten meanwhile. Publication now invokes a ready
callback after releasing the worker mutex. The callback notifies the renderer
under its wait mutex, and the wait predicate checks for a ready result. The
renderer consumes a completed result independently of submitting a new decoded
frame, retaining the latest-frame queue and existing freshness/resolution rules.

The native regression submits exactly one frame, waits for a ready notification,
re-enters `has_result()` from the callback, and takes that result without sending
another frame. This checks result delivery and callback lock ordering. Real
post-change display cadence still needs a repeat of the same live test; unchanged
model execution does not itself guarantee a particular display FPS.



## HyperSwap B: TensorRT builder level 5

TensorRTModel accepts an explicit `builder_level` in 0..5, defaults to 3 and
includes it in the engine fingerprint and ORT provider options. Default level-3
cache keys remain compatible. HyperSwap B now selects level 5 for the ordinary
TensorRT FP32 option, retrying level 3 on build/warmup failure before CUDA.
Other swapper models, the detector and GPEN retain level 3; the experimental
TensorRT FP16 selection also retains its prior level. Precision is unchanged:
HyperSwap's original FP16 weights/internal graph and float32 IO are preserved,
with TensorRT FP16-enable still false, workspace 2 GiB and auxiliary streams 0.

RTX 3080 Laptop comparison: 120 paired frames per level and fixture, alternating
execution order, same fixed detector TensorRT engine, fused metadata path,
identity and changing GPU-held images. Initial level-5 build/load took 487.99 s;
a second run loaded it in 2.46 s. Compilation was excluded from measurements.
Confirmation run:

| Fixture | Level 3 median / P95 | Level 5 median / P95 | Pipeline throughput ratio |
|---|---:|---:|---:|
| 498x1080, one face | 10.619 / 11.550 ms | 6.649 / 7.421 ms | 1.597x |
| 1280x720, six faces | 10.596 / 11.686 ms | 6.629 / 7.515 ms | 1.599x |

Swapper-stage medians fell from 9.030 to 5.132 ms and 8.999 to 5.070 ms,
respectively. These fixtures exclude AirPlay transport, D3D11 texture handoff,
rendering and presentation; the speedup is not a claim of 1.6x live display FPS.

Face counts remained identical, no-face output remained unchanged, and changing
inputs plus texture resolution changes were verified. Full-frame pixel deltas
were at most 1/255 on the portrait fixture and 2/255 on the six-face fixture;
mean deltas were below 0.0017 byte levels including alpha. Raw aligned model
output mean differences were below 0.055 byte levels, with maxima below 2.72.
The diagnostic rejects nonfinite output or excessive fixture differences.
A production-constructor check selects level 5 automatically and verifies its
portrait result against the measured cached engine.

The initial managed GPU warmup timeout was extended from 10 to 20 minutes to
accommodate level-5 building on slower systems; normal frame deadlines and
cancellation checks are unchanged. The observed timings/quality are specific
to these fixtures and this GPU. Future model/GPU combinations need validation.

```powershell
.\.venv-face-swap\Scripts\python.exe tools/benchmark_face_swap_builder.py `
  --models D:/models --source D:/photos/source.png --target D:/photos/target.jpg `
  --model hyperswap_1b_256 --frames 120 --output build/tensorrt-builder/hyperswap-b
```

## XSeg 1 occlusion trial

The optional GPU-resident mask uses original BGR NHWC 256x256 pixels, TensorRT
FP32 with CUDA fallback, on-device filtering and alpha combination. It runs
before swapping; GPEN uses the same visibility mask. The default remains off.
Paired alternating 100-frame still-fixture measurements (HyperSwap B level 5,
RTX 3080 Laptop): off median 6.846/P95 7.982 ms, XSeg median 11.490/P95 12.362 ms.
Overhead: +4.644 ms; excludes D3D11, AirPlay and presentation. Artifact:
`build/xseg-benchmark/results.json`; reproduction:
`python tools/benchmark_face_swap_occlusion.py`. Live moving hands still require
user validation; the still output mask only validates face/background separation.

## Alternative detector trial

SCRFD's optimized 320 path remains the default. YOLOFace 8n and YuNet use the
publisher's fixed 640 exports; RetinaFace is specialized to 320. CUDA packs BGR
inputs, runs fixed GPU IO through TensorRT FP32 (CUDA fallback) and decodes
YOLOFace/YuNet to the common 15-float candidate format before one metadata
transfer. RetinaFace reuses the existing fused FPN decoder. CPU/DirectML use
equivalent preprocessing, decoding and NMS. The detector applies to source
identity extraction and the live image, so changing it can change the source
embedding and alignment as well as processing time.

The smoke benchmark writes build/detector-comparison/results.json; it verifies
a detected face and clears candidates on a blank frame. Those sequential times
were gathered under other GPU activity and different model input resolutions;
do not use them as an isolated accuracy/speed ranking. Managed GPU fixtures
validate texture attachment, output and resolution changes for all alternatives.
