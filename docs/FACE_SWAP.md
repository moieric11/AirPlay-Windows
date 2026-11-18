# Face swap: NVIDIA CUDA, DirectML and CPU

## Automatic Python preparation

For a release ZIP on a fresh Windows PC, select **Processing device** in
**Faceswap Settings**, then click **Prepare Python Environment** below the
engine status. The application downloads and verifies an official Python 3.11
x64 package and installs dependencies for NVIDIA CUDA, DirectML or CPU in a
dedicated local environment. NVIDIA preparation also installs TensorRT when
its options are enabled, and can require several GB of downloads.

The interface stays responsive. Preparation can be cancelled, and errors have
an accessible log. The interpreter is checked before and after relocation,
then its path is selected and saved automatically. CUDA, DirectML and CPU
environments are separate. Existing custom environments are preserved.

No models are downloaded by this button. Supply the required original models
or use the model download controls after Python is ready. GPU drivers must be
installed separately. The manual environment instructions below remain useful
for custom installations and development.

## Experimental reference animation (LivePortrait)

In **Faceswap Settings > Model & quality > Rendering engine**, select
**Reference animation - LivePortrait (experimental)**. Download/verify the
LivePortrait pack, choose a clear frontal source photo with open eyes, and start
the engine. This mode requires NVIDIA CUDA and **Keep video on GPU**. It does
not need the HyperSwap, ArcFace or GPEN models. Stop the engine to change modes.

The photo's appearance volume and 21 canonical 3D points are computed once.
The current frame supplies pose and expression directly. With HRFFA enabled,
rigid anchors (nose, chin, eye and mouth corners) from its 68-landmark output
provide absolute head pose through perspective pose estimation. Invalid poses
fall back to LivePortrait's current neural pose. There is no neutral capture,
pose anchor, manual calibration or first-frame expression delta. The photo's
canonical geometry stays fixed; the video supplies the current expression.
LivePortrait generates a 512-pixel image through the existing blending masks.
**Animation strength** updates live; 0% retains the photo's pose/expression.
HRFFA also refines crop alignment. Mask, padding and preview controls stay live.

When HRFFA is enabled, the generated 512-pixel face is measured as well. A
similarity transform matches its eye centres, nose and mouth corners to the
target landmarks expressed in the same crop. CUDA resamples the generated
image before blending, correcting global position, scale and in-plane rotation
while preserving donor proportions. This shares the existing HRFFA session;
one extra inference and 544 bytes of landmark metadata are needed per frame.
Image pixels remain on the GPU and target landmark diagnostics are restored.
Generated landmark validation permits movement away from the static photo's
points. Transform limits use displacement of the feature centroid, rather than
translation about image origin. Scale and roll are bounded while keeping that
centroid on the current target. If target HRFFA landmarks are rejected, detector
five-point anchors still supply registration. If generated landmarks or the
transform remain unreliable, the original video is shown for that frame;
an unregistered generated face is never pasted. No calibration or temporal
reference is captured. Logs report `registered`, `registration_status` and `registration_error_px`
(five-point RMS before/after, in 512-pixel crop coordinates). Manual Face scale
and Face size apply afterwards; use 100% for both when checking alignment.

For the initial comparison, use face size/scale 100%, no occlusion or region
preservation. Preserving original eyes/mouth deliberately retains those video
pixels. This is experimental: difficult poses, lighting and newly revealed
areas may show artifacts, and animation is more expensive than face swapping.

Four pinned ONNX files (about 338 MB) live in `face_swap/swappers`; their hashes
and publisher metadata are in the model manifest. They are excluded from the
release archive. Each model follows the selected TensorRT builder level 3/5,
with CUDA IO Binding/graphs if TensorRT cannot build it. Image generation stays
on the GPU. First-time compilation can be cancelled through the engine controls.

Motion equations follow the MIT-licensed
[KwaiVGI LivePortrait project](https://github.com/KwaiVGI/LivePortrait).
The ONNX exports are pinned from the FaceFusion model repository; see the
manifest for individual URLs, revisions and license metadata.

Choose **Processing device > CPU** in **Faceswap Settings** to run detection,
identity encoding, swapping and optional GPEN restoration without CUDA or an
NVIDIA GPU. NVIDIA remains the default. Stop face swap before changing device.
CPU mode bypasses CUDA Graphs, GPU texture sharing and TensorRT even if their
preferences remain checked. ONNX Runtime must provide CPUExecutionProvider;
either onnxruntime or onnxruntime-gpu can supply it. The Python environment and
local model files are still required.

Start with InSwapper / Fast or HyperSwap without restoration. CPU frame rate is
hardware dependent; GPEN and AlphaFace can be substantially slower. The app
keeps only the latest pending frame and holds completed CPU results between
updates (up to 60 seconds), rather than discarding every frame above the GPU
500 ms deadline. This is a low-frame-rate test mode, not a real-time guarantee.
The diagnostics report CPU and the measured frame processing time.
For a standalone worker, pass `--backend cpu`. AMD/Intel GPU inference is not
implemented by this CPU mode; video decoding may still use its own GPU setting.


## DirectML on Windows

Select **Processing device > DirectML (AMD / Intel / NVIDIA)**, then choose the
DXGI graphics adapter by name. Stop face swap before changing the backend.
This uses DirectML for detection, ArcFace, the selected swapper and optional
GPEN. CUDA Graphs, TensorRT and CUDA texture sharing are bypassed. Video pixels
currently pass through RAM; this path does not provide GPU-resident display.
Video decoding has its own independent GPU setting in the standard Settings page.

Use a separate environment, because `onnxruntime-directml` and
`onnxruntime-gpu` install the same Python module and must not be mixed:

```powershell
py -3.11 -m venv .venv-face-swap-directml
.\.venv-face-swap-directml\Scripts\python.exe -m pip install -r tools/requirements-face-swap-directml.txt
.\.venv-face-swap-directml\Scripts\python.exe -m pip install --no-deps insightface==2.1
```

The app discovers this environment from the repository, or the portable
`face_swap/python-directml/python.exe` beside the executable. Override it using
**Engine & diagnostics > DirectML Python executable**. CUDA and CPU keep their
existing Python path. Backend, DirectML adapter index and runtime path are saved;
old `face_swap_cpu=1` preferences migrate to CPU mode.

HyperSwap A/B/C require an additional DirectML precision correction: their
published graphs contain FP16 weights, constants and internal casts. On the
RTX 3080 Laptop, DirectML evaluation of these half-precision graphs noticeably
distorts the face, even with identical input tensors and near-identical source
embeddings. DirectML now uses a derived FP32 graph (weights, constants, casts
and intermediate type annotations), cached by source SHA-256 and conversion
version. Original models, CUDA/TensorRT and CPU behavior are preserved. The
cache is about twice the original model size; first use prepares it locally.
Fixed-input comparisons of all three variants against original CUDA output
showed a maximum channel difference below 0.4 on the 0..255 scale on the test
fixture. AMD accuracy still requires testing on AMD hardware.

DirectML requires sequential ORT execution and disabled memory patterns.
SCRFD uses distinct fixed-shape sessions for source (640) and live (320)
detection; a single dynamic DirectML session fails when switching resolutions
with the supplied detector. Provider activation is checked for every model,
including derived detection sessions; errors do not silently switch the entire
session to CPU. ORT may still assign unsupported graph operations to CPU.
Diagnostics say `DirectML [CPU pixels]` and show elapsed frame time. A five-second
freshness/hold limit accommodates first-use compilation without an unbounded queue.

Validation on the RTX 3080 Laptop: real source/target frames processed with
InSwapper, HyperSwap A/B/C, AlphaFace and GPEN 256/512. ORT profiling confirms
DirectML kernels for detection, recognition, swapping and restoration.
These NVIDIA tests do not validate AMD drivers or predict Radeon performance.
For a standalone worker, use `--backend directml --device <DXGI adapter index>`.
Reference: [ONNX Runtime DirectML requirements](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html).


The mirror preview can replace the largest detected face with a face from a
local photo. Open **Faceswap Settings** at the bottom of the navigation, directly
above **Settings**, choose a photo, then
enable the effect. If the photo has several faces, its largest face is used.
The target is selected by size each frame; it is not identity tracking, so it
can change when another person moves closer to the camera.

The app starts its selected engine automatically when you enable the effect.
No terminal or PowerShell launcher is needed for normal use. Choose the photo
and models folder once; the photo, models folder and Python runtime path are
saved with the other settings. The enable checkbox starts off on app launch.
Disconnecting the AirPlay session disables the effect and stops its engine.

The engine still runs in a managed Python subprocess, rather than embedding
Python or CUDA inference inside the native renderer. It has no console window,
uses its own available loopback port, and cannot conflict with a manual worker
on port 7011. Disabling the effect or closing the app stops its child process.
A Windows job also cleans up the engine if the receiver crashes.

## NVIDIA CUDA setup

Use a separate **Python 3.11 x64** environment and an NVIDIA GPU/driver compatible
with ONNX Runtime's CUDA provider. From the repository root:

```powershell
py -3.11 -m venv .venv-face-swap
.\.venv-face-swap\Scripts\python.exe -m pip install -r tools/requirements-face-swap.txt
.\.venv-face-swap\Scripts\python.exe -m pip install --no-deps insightface==2.1
```

Install InsightFace separately with `--no-deps`: its default dependency is CPU
`onnxruntime`, which shares package files with `onnxruntime-gpu`. All the runtime
dependencies needed here are already included in the requirements file.

Provide these compatible ONNX files locally:

- SCRFD face detector with five landmarks, such as `det_10g.onnx` from buffalo_l.
- ArcFace recognizer, such as `w600k_r50.onnx` from the same pack.
- `inswapper_128.onnx` or `inswapper_128.fp16.onnx`.

Put the three files in one folder, select it with **Choose models folder...**,
choose your source photo, and enable face swap. Loading happens in the
background while the app continues showing live video. Missing dependencies,
unreadable photos and photos without faces appear in the panel; use **Retry
face swap** after correcting the problem.

**Quality** offers three modes (disable the effect before changing modes):

| Mode | Video limit, longer side | Face restoration |
|---|---|---|
| Fast | 1280 pixels | None; original InSwapper output |
| Balanced (default) | 1920 pixels | `GPEN-BFR-256.onnx` |
| Detail | 1920 pixels | `GPEN-BFR-512.onnx` |

The restoration model must be in the same models folder. **Restoration
strength** blends the restored detail with the swapped face; the default is
60%. GPEN works only on the aligned face crop, using a feathered mask when
putting it back. It does not sharpen the full frame. Increasing strength can
alter fine facial features, and cannot recover genuine details absent from
InSwapper's 128-pixel generated face. See the [GPEN project](https://github.com/yangxy/GPEN)
for model information and licensing.

On one local 564x554 photo fixture, warmed inference measured about 43–50 ms
without restoration, 65–70 ms with GPEN 256, and 170–200 ms with GPEN 512. These
figures include detection, swap and restoration, exclude AirPlay transport and
display, and do not predict live full-HD frame rate. Select Fast if GPU load
makes the effect too slow.

For a source build, the app finds `.venv-face-swap/Scripts/python.exe` in the
repository automatically. **Faceswap Settings > Engine & diagnostics > Python executable** lets you
choose another provisioned Python environment. For a standalone installation,
the default layout is `face_swap/python/python.exe` and `face_swap/swappers/`
beside the receiver executable. CMake deploys `face_swap/engine/face_swap_worker.py`;
Python, CUDA libraries and model weights require one-time provisioning and are
not bundled by the normal receiver build. A local `face_swap/models-path.txt`
file may contain the UTF-8 path to an existing models folder when app-local weights are absent; saved UI settings
take precedence. Installation paths are resolved relative to the executable,
not the working directory used to launch the app.

Model weights are not bundled or downloaded automatically. InsightFace's code
and pretrained weights have different licensing terms; check the licenses for
your chosen weights before distributing or using them commercially. See the
[official model information](https://github.com/deepinsight/insightface/tree/master/python-package)
and [swap example](https://github.com/deepinsight/insightface/tree/master/examples/in_swapper).

**Copy engine log path** copies the current log filename. By default, logs live
in `face_swap/logs`. They report source detection,
target face count and processing time every two seconds. Loading errors include
the log path in the panel. The status reports CUDA only after checking all three
model sessions; unsupported individual operations may still execute on CPU.

`tools/start_face_swap.ps1` and direct `face_swap_worker.py` invocation remain
diagnostic tools. They start independent engines and are no longer part of the
application's normal startup. Stop a previously launched manual engine using
its printed PID to avoid keeping an extra set of models in GPU memory.

The optional ONNX Runtime CUDA/cuDNN extras supply runtime libraries; their
availability depends on the Python/platform combination. See the official
[CUDA compatibility and DLL loading instructions](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html)
if initialization fails. Remove a separately installed CPU `onnxruntime` package
from this environment if it conflicts with `onnxruntime-gpu`.

## GPU-resident preview (experimental)

Enable NVIDIA GPU decoding and **Keep video on GPU** before
starting face swap. This option is saved with the other settings. The status
`[GPU resident, no image readback]` confirms that video pixels stay on the GPU.
The original preview continues during model and kernel initialization.

This path needs the optional native `face_swap/engine/airplay-cuda-bridge.dll` and
CuPy in the selected Python environment:

```powershell
.\.venv-face-swap\Scripts\python.exe -m pip install -r tools/requirements-face-swap-gpu.txt
```

For source builds, CMake builds the bridge when the NVIDIA CUDA Toolkit is
available and `ENABLE_CUDA_FACE_SWAP=ON` (the default). The bridge uses the CUDA
Driver API; CuPy and ONNX Runtime use their CUDA 12 runtime libraries.

D3D11 converts and resizes the decoded texture on the GPU. The bridge copies
it to a CUDA buffer on the same NVIDIA adapter. CUDA kernels perform detector
preprocessing, face alignment, blending and restoration preprocessing; ONNX
Runtime uses persistent device buffers through IO Binding and CUDA Graph replay
for compatible models. Capture happens during initialization; rejected capture
rebuilds that model's session with CUDA IO Binding, keeping its precision and
device buffers. A later execution failure uses the normal pipeline recovery.
The processed
image returns to a D3D11 texture for the existing GPU renderer. The socket
carries shared-texture handles and acknowledgments, without image payloads.
Detection metadata and landmark coordinates reach the CPU for face selection
and transform calculation. SCRFD's outputs are decoded by one CUDA kernel into
persistent buffers and copied once to pinned host memory. For the supplied
320-pixel detector, this is a fixed 252,000-byte metadata block per frame, about
15.1 MB/s at 60 frames/s, with no image pixels. The fixed transfer avoids dynamic
candidate-count readbacks and preserves every anchor in its original order;
confidence filtering and NMS retain their existing semantics. Initial source-photo
analysis is separate from the live video path.

Device-to-device copies and explicit synchronization remain: this avoids CPU
pixel transfers, but does not eliminate copies or GPU waits. GPU sessions
disable CPU execution-provider fallback. The detector's fixed 320-pixel input
shape is specialized without editing the original model. Enabling TensorRT for
the swapper or restoration also enables a compatible SCRFD 320 TensorRT FP32
engine. Its derived ONNX graph and engine are cached alongside the existing
model/engine caches. If its build or initial inference fails, detection keeps
CUDA Graphs and persistent GPU buffers. Live logs identify `detector=TensorRT FP32`
or `detector=CUDA`; CPU and DirectML paths are unaffected. Resolution changes
reuse model sessions on the same adapter. The GPU blending mask and interpolation
can produce small visual differences from the compatibility path.

If the bridge, CuPy, NVIDIA decoding or texture sharing is unavailable, the
engine uses the compatibility path and reports `[CPU pixels]` with the fallback
reason. Disable and re-enable face swap to retry the GPU path after changing
its configuration.

## TensorRT restoration (optional)

**Balanced** and **Detail** can use **TensorRT restoration** when the
GPU-resident option is enabled. This saved option defaults on; installations
without the optional TensorRT runtime keep CUDA Graph restoration. Balanced uses
GPEN 256 and Detail uses GPEN 512, both in FP32. Fast retains its existing engine.
GPEN still runs on every processed face.

For the pinned Windows ONNX Runtime 1.23.2 environment, install the optional
TensorRT 10.9 CUDA 12 libraries using the official NVIDIA wheel in:

```powershell
.\.venv-face-swap\Scripts\python.exe -m pip install -r tools/requirements-face-swap-tensorrt.txt
```

The native app still manages the Python worker automatically. TensorRT uses
FP32 mode, device IO Binding and its graph support. The engine cache is under
`face_swap/swappers/tensorrt-engines` for the default
installation. Cache directories include a fingerprint of model contents,
ONNX Runtime and TensorRT versions, GPU name/architecture, CUDA driver API
version, precision and builder settings. Model weights are never rewritten.
In **Faceswap Settings > Processing device**, enable NVIDIA CUDA, Keep video
on GPU to show **TensorRT compilation (all models)**:

- **Level 5 (slower first build, faster processing)**: default, more exhaustive build.
- **Level 3 (faster first build, slower processing)**: shorter compilation/reference path.

Legacy Automatic settings migrate to level 5. The choice is saved, resets to level 5 with Reset all settings, and can be
changed while the engine is stopped (or after a failure). It applies to every active TensorRT model: swapper, SCRFD/YOLOFace/RetinaFace/YuNet detector, XSeg, BiSeNet, HRFFA and GPEN 256/512. CPU/DirectML ignore it.
A level-5 gain is validated here for HyperSwap B on the tested GPU, not for every
model/GPU combination. The standalone worker accepts `--swapper-builder 0|3|5`.
 Precision flags, workspace and auxiliary streams are unchanged. If
a TensorRT build/warmup fails, that model uses CUDA and logs the failure; it never silently builds at a lower level. Engines
for each model and level have separate cache keys. Preparation reports and model-ready logs show the actual builder level.
The first level-5 build took about eight minutes on the tested RTX 3080 Laptop;
subsequent loading used the cache. The managed initial GPU warmup allows up to
20 minutes while remaining cancellable through Stop faceswap.


The first FP32 builds took about 83 seconds for GPEN 256 and 189 seconds for
GPEN 512 on the local RTX 3080 Laptop GPU. The app keeps showing original video
while it builds; initial GPU preparation allows up to twenty minutes
with cancellation still available. Subsequent sessions
load the cached engine. Initialization failure falls back to CUDA restoration
and explains the reason in the engine log. Preparation starts after source identity calculation, before any video frame is
submitted. Textures are attached later without recreating model sessions. Logs report
`restoration=TensorRT FP32` when active, or `restoration=CUDA` on the CUDA path.

The local 1280x720 Balanced fixture improved from 33.232 to 22.064 ms median for
GPU image processing (60 frames, two alternating photo fixtures), with maximum
output difference one level on a 0-255 scale. This excludes AirPlay, D3D11
transport and display, and is not a live frame-rate guarantee. FP16 was also
tested but produced substantially larger image differences, so it is not used
by the application. See the [measurements and reproduction](FACE_SWAP_OPTIMIZATION.md).

For Detail, the same 1280x720 fixture improved from 136.376 to 67.197 ms median,
again with maximum output difference one. GPEN 512 remains substantially more
expensive than GPEN 256. To try it, disable face swap, choose **Detail**, keep
**TensorRT restoration** checked and enable the effect again.

## Behavior and limits

Experimental AlphaFace and HyperSwap native 256 models can now be selected
without GPEN. InSwapper remains the default. See [model setup and comparison](FACE_SWAP_MODELS.md)
for adapters, provisioning, benchmark limits and instructions for live trials.

In the compatibility path, inference runs through CUDA while pixel downloads,
resizing, face alignment, blending and local transfers use CPU memory. In the
GPU-resident path described above, video pixels remain on the GPU. Both paths
resize frames to at most 1920 pixels on the longer side
(1280 in Fast mode),
with even dimensions, preserving the aspect ratio to within rounding.
Processed frames are presented directly as RGB, avoiding an additional
YUV420 chroma subsampling step after restoration. Input conversion follows the
received frame's BT.601/BT.709 matrix and full/limited range metadata.

There is one pending input slot and one completed output slot. New pending
frames replace older ones; results older than 500 ms are discarded. The preview
holds the last swapped frame between results and returns to live original video
while starting, when the worker fails, or after 500 ms without a fresh output.
A scene with no detected face is returned unchanged. Balanced and Detail apply
GPEN face restoration after swapping. Performance depends on resolution, model and GPU; no real-time frame
rate is guaranteed. CUDA model loading, source detection and swap inference have
been checked locally with SCRFD, ArcFace and `inswapper_128.fp16.onnx`; live
AirPlay frame rate still depends on the scene and resolution.

Only the **mirroring preview** is affected. HLS playback and MP4 recordings keep
their original images and audio. The managed engine listens only on `127.0.0.1`
and keeps the source embedding in memory until the client disconnects. Do not
expose its port through a proxy; the protocol is
intended for trusted processes on the same computer.

## Verification

The compatibility worker uses CUDA Graphs and persistent GPU buffers where
supported, and limits swap paste-back to a padded face crop. The resident worker
uses persistent device IO Binding, CUDA Graph replay and CUDA preprocessing/blending kernels.
Model precision remains unchanged.
See [optimization notes and reproducible benchmarks](FACE_SWAP_OPTIMIZATION.md).

```powershell
python -m unittest discover -s tools -p test_face_swap_worker.py
cmake --build build --config Release --target test-face-swap
$env:FACE_SWAP_TEST_EXE = "$PWD\build\Release\test-face-swap.exe"
python -m unittest discover -s tools -p test_face_swap_worker.py
```

Capture-buffer reuse and CUDA-only fallback can also be checked without GPU
hardware using the face-swap environment:

```powershell
.\.venv-face-swap\Scripts\python.exe -m unittest discover -s tools -p "test_face_swap*.py"
```

The native integration target is enabled by `-DBUILD_GPU_TESTS=ON`. It uses a
fake worker and CPU fixture to validate framing, conversion, source rejection,
disconnect/error handling, startup cancellation, and child process cleanup on
normal and abrupt parent exit without model weights. Native process launch
also covers model paths with spaces and accented characters. Actual CUDA
inference and NVIDIA performance require the locally provided models.

`test-gpu-face-interop` additionally checks real D3D11/CUDA texture exchange,
channel values and texture resizing on NVIDIA hardware. With Python, worker,
model-directory and photo arguments, it exercises the managed process and real
GPU inference, including a resolution change without model-session recreation:

```powershell
cmake --build build --config Release --target test-gpu-face-interop
.\build\Release\test-gpu-face-interop.exe `
  "$PWD\build\Release\face_swap\engine\airplay-cuda-bridge.dll" `
  "$PWD\.venv-face-swap\Scripts\python.exe" `
  "$PWD\build\Release\face_swap\engine\face_swap_worker.py" `
  "D:\models" "D:\photos\source.jpg"
```

The full path was checked locally on an RTX 3080 Laptop GPU, including a real
photo texture and GPEN 256/512. Test-only readbacks validate output pixels; they are
not part of the production path. Live AirPlay rendering, frame rate and temporal
quality still require a live session.

### Preparation and cancellation

The Live preview card shows the actual preparation stage, TensorRT model and
builder level, elapsed time, and an animated activity bar. ONNX Runtime's
TensorRT provider does not expose IProgressMonitor callbacks, so no estimated
0-100% is presented as measured compilation progress. Existing engine files are
reported as cache loading/checking; the provider may rebuild incompatible caches.
Cancel preparation uses the same managed-job shutdown as Stop faceswap: it stops
the worker and its children, including an ongoing build, and preserves model
weights and completed engine caches. The interrupted build may need to restart.

Stopping screen mirroring disconnects video input without stopping the face-swap
worker. An ongoing TensorRT build continues and the completed model sessions are
reused when mirroring reconnects. Pending and completed video frames are dropped;
results from an old session cannot be shown in a new session, even at the same
resolution. Cancel preparation, Stop faceswap, Reset all settings and quitting
the app still stop the engine. GPU-resident preparation starts on activation,
after loading the source identity, without any screen mirroring. The GPU model
sessions and fixed IO buffers are retained until video textures are connected.

### App-local model storage

All production caches are rooted beside the worker, independent of its working
directory and of the logs folder:

- `face_swap/swappers/`: original ONNX weights (detector, recognizer, swappers, GPEN).
- `face_swap/swappers/prepared/`: derived static/SCRFD/DirectML ONNX graphs.
- `face_swap/swappers/tensorrt-engines/`: versioned TensorRT engines and timing caches.
- `face_swap/swappers/cuda-kernels/`: compiled CuPy kernels.
- `face_swap/logs/`: worker logs, preparation stages and cadence metrics.

The standalone worker accepts `--cache-root` to choose a different swapper root.
Cached engine keys are unchanged by relocation, so existing compatible caches
remain usable. The local old `%LOCALAPPDATA%/AirPlay-Windows/face-swap-logs`
cache directories were moved and removed; original shared VisoMaster weights
were copied into the app and the external library was preserved.

### GitHub release layout

Worker scripts, requirements, the model manifest/downloader and optional CUDA
bridge are deployed to `face_swap/engine`. The native launcher resolves the
adjacent `face_swap/swappers` folder explicitly; caches stay outside `engine`.
For local packaging, run `tools/package_release.ps1 -Version preview`. The same
script is used by `.github/workflows/release.yml`. It includes only the runtime
files required for distribution, plus documentation and notices. It excludes
local Python environments, source photos, model weights, GPU caches, logs,
`models-path.txt`, receiver identity, test executables and debug symbols.
The ZIP and SHA-256 are generated under `build/packages` by default.
See [distribution setup](FACE_SWAP_DISTRIBUTION.md) for provisioning the optional
Python/model runtime; this is not yet a completely self-contained faceswap ZIP.

### Model management

With face swap stopped, select AlphaFace or HyperSwap A/B/C in Model management.
The card displays the model's reported license and a
Model terms link. Review the terms, check the acknowledgment, then select
Download / Verify. The helper uses the configured Python executable (standard
library only) and downloads from a pinned revision on FaceFusion's official
Hugging Face account. Target: `face_swap/swappers`, independent of the separate
base-model folder. Download **ArcFace (w600k_r50.onnx)** from Model management
into the configured models folder. Install InSwapper and GPEN separately. Use **Prepare
Python Environment** to provision Python and its dependencies.

An existing matching file is verified and reused. New bytes are written to a
private partial file and checked against pinned size and SHA-256 before atomic
replacement. The progress bar measures transferred bytes; checking and verifying
use an indeterminate bar. Cancel stops the owned downloader job and cleans its
partial file, preserving the previous model. Failed authentication/access is
reported without bypassing publisher gates. The model's usage restrictions still
apply. No inference runs while a download is active.

### Experimental occlusion protection (XSeg 1)

In Processing device, select NVIDIA CUDA and Keep video on GPU, stop faceswap,
then enable **Occlusion protection - XSeg (experimental)**. It is off by default
and persisted independently of model choice. Download / Verify XSeg 1 provisions
the pinned official FaceFusion Hugging Face export in `swappers/xseg_1.onnx`
(SHA-256 checked; GPL-3.0 per the publisher's metadata). CPU, DirectML and CPU
pixel transport currently do not apply this option.

XSeg runs on the original aligned BGR crop before swapping. Batch-1 ONNX lives
in `swappers/prepared`, TensorRT FP32 engines at the selected builder level in `tensorrt-engines`, with
CUDA IO Binding/graphs as fallback. Clipping, separable Gaussian filtering and
combination with the ellipse remain on GPU. The same visibility mask is resized
for GPEN so restoration cannot overwrite protected regions. Mask is recomputed
per processed face; no stale masks are reused across frames or sessions. No face
means no XSeg inference. Logs report `occlusion` backend and a wall-time stage.

A paired 100-frame-per-mode GPU still-image fixture with HyperSwap B level 5 on
the local RTX 3080 Laptop measured median 6.846 ms off vs 11.490 ms on (+4.644 ms),
P95 7.982 vs 12.362 ms. First preparation/build was 65.9 s (includes all GPU models).
These exclude AirPlay, D3D11 interop and display; no live hand-occlusion quality
claim is made from this still fixture. The masks do not stabilize identity or
landmarks temporally. Reproduce with `tools/benchmark_face_swap_occlusion.py`.

### Experimental BiSeNet face regions

NVIDIA CUDA + Keep video on GPU exposes Face regions - BiSeNet ResNet18 in
Processing device. Default Off; presets are Face regions, Preserve original
eyes, Preserve original mouth, and Preserve both. Download / Verify BiSeNet
installs the pinned official FaceFusion export (MIT per publisher metadata).
CPU/DirectML and CPU pixel transport do not apply this option.

The parser runs on the original aligned face: RGB NCHW 512x512 with ImageNet
normalization, 19 semantic classes. Selected face labels are skin, eyebrows,
eyes/glasses, nose, mouth and lips. The eye preset excludes labels 4/5/6; the
mouth preset excludes 11/12/13. Preserved pixels retain the original identity.
This is semantic region masking, not eyelid tracking: a closed eyelid classified
as skin may still be swapped. Live expression improvement is not guaranteed.

Preparation keeps the primary logits and prunes unused auxiliary outputs; the
original model remains intact. TensorRT FP32 at the selected builder level is cached, with CUDA IO
Binding/graphs fallback. Argmax, filtering, resize and mask composition stay on
GPU. Parser and optional XSeg both inspect the original frame before swapping;
alpha is ellipse * XSeg * selected regions, including GPEN restoration. Logs
report regions backend and stage. No-face frames skip both mask inferences.

Paired 100-frame GPU still fixture, HyperSwap B level 5, parser-only eye mode:
median 6.687 ms off vs 9.459 ms on (+2.772 ms); P95 7.482 vs 10.908 ms. Includes
all GPU processing but excludes AirPlay, D3D11, sockets and display. First model
preparation/build took 22.55 s in this run. Artifacts: build/bisenet-benchmark.
Reproduce with `python tools/benchmark_face_swap_occlusion.py --parser-mode 2
--no-xseg --output build/bisenet-benchmark` (one command).

### Blending mask shape, margins and diagnostic preview

In NVIDIA CUDA + Keep video on GPU, Processing device exposes Mask shape
(Ellipse or Box), independent Top/Right/Bottom/Left margins (0..45% of the aligned
crop per side), and Edge softness (0..50%). Positive margins shrink the replaced
area on that side; they do not rescale, move or correct generated facial features
or alter detection landmarks. Defaults reproduce the previous ellipse exactly:
zero margins and 18% softness. Reset blending mask restores these defaults and
turns the preview off. Settings are saved and these geometric controls apply live; models and device options still require stopping faceswap.

The geometric mask is computed during preparation and when its geometry changes, for swap and GPEN sizes.
XSeg and BiSeNet multiply this base mask as before; no extra per-frame inference
or CPU image transfers are added. CPU, DirectML and CPU pixel fallback retain
their existing blend path; these controls currently apply to GPU-resident CUDA.

Preview in screen mirroring offers Off, Mask overlay + alignment points, or
Mask only + alignment points. Green indicates effective swap alpha after
XSeg/BiSeNet; red dots mark the five source-video landmarks used for alignment.
Mask-only is grayscale alpha over black with the same points. Both preview
modes change the displayed output and run entirely on GPU; recordings still
use the original stream. No-face frames clear the mask-only view and never reuse
old landmarks. Disable preview for the normal image and maximum throughput.

### Face detector selection

Faceswap Settings > Face detection selects the detector used for the source
photo and live stream. The default remains SCRFD 10G (existing det_10g.onnx):
640 input for source identity preparation, 320 for live processing. Optional
alternatives are YOLOFace 8n (640), RetinaFace 10G (320) and YuNet (640). These
are separate detectors, not swappers. Their different fixed input resolutions
are shown in the UI; timings are not an equal-resolution accuracy comparison.

Stop faceswap and select the detector in **Face detection**, where the face
landmarker is also configured. In **Model management**, select the active detector
or another model under **Model to download**, review
the displayed publisher terms, then click **Download / Verify**. SCRFD is extracted
from the official InsightFace `buffalo_l` archive into the configured models folder.
Other detectors are verified against pinned official FaceFusion revisions and
installed in `face_swap/swappers`.
YOLOFace is labelled GPL-3.0, RetinaFace non-commercial, YuNet MIT, per publisher
metadata. Runtime, model selection, terms display and downloads are separate:
downloading does not grant additional model rights.

All selections support the CPU and DirectML pixel paths. CUDA GPU-resident
processing specializes ONNX IO, uses TensorRT FP32 at the selected builder level with CUDA IO Binding
fallback, and keeps image pixels on GPU. Each alternative has its own input
normalization and output decoder. A single pinned-host transfer contains only
detection metadata per frame (RetinaFace 252,000 bytes; YOLOFace/YuNet 504,000).
Default SCRFD's fused path and engine-cache keys are retained. Letterbox scaling
is inverted per axis for the new models. Source and live decoders share a
five-point/NMS contract, and no-face frames clear previous detections.

The local models passed CPU and DirectML detection on the portrait fixture;
all three also passed managed D3D11/CUDA/TensorRT processing, headless model
preparation and resolution changes. Decoder tests compare synthetic GPU output
against CPU formulas and verify stale-result clearing. The still benchmark
tools/benchmark_face_swap_detectors.py writes build/detector-comparison/results.json.
Its timings were collected with other GPU activity and are diagnostic only;
they are not a definitive quality or speed ranking. Use the mask/landmark
overlay and live logs to compare stability on the same movements.

### Live geometric mask controls

Shape, Top/Right/Bottom/Left margins, edge softness, diagnostic preview and Reset
blending mask can be changed while the GPU engine is running. A miniature of the
live video appears alongside those controls, using the existing display texture
and rotation. There is no extra image download. Model/device controls stay locked.

Changes use a separate acknowledged loopback control packet, serialized with
frame processing on the native worker thread. Only the newest unsent settings
are retained; no unbounded queue or per-frame disk polling is introduced. The
control wakes an idle engine without requiring a new video frame. During initial
model preparation it is queued and applied after readiness, before pending video
frames. Mask arrays are replaced between frames; model sessions, CUDA Graph IO
and TensorRT engines remain loaded. Preview toggles reuse the prepared kernels
and existing geometric arrays. These controls currently apply to GPU-resident
CUDA; CPU/DirectML pixel transport retain their previous blending behavior.

Face scale (80-120%, default 100%) adjusts generated facial pixels around the aligned crop centre inside the unchanged target mask. It is saved and updated live on the GPU-resident CUDA path, without changing model inputs, TensorRT engines or target-aligned XSeg/BiSeNet masks. GPEN restores the already scaled result and does not apply the zoom again. Start around 102?105%; this also moves eyes and mouth and cannot correct every alignment or expression mismatch.

Face size (global) (80-120%, default 100%) resizes the entire composited face and its effective geometric/XSeg/BiSeNet mask together about the aligned crop centre. Face scale remains a separate zoom of generated pixels within that mask; both controls combine and update live on CUDA. GPEN samples the globally resized region and restores it at that same placement, so global size is applied once. Mask diagnostics follow the resized placement; detected alignment points still indicate the original target landmarks.

The Faceswap engine card contains status and start/stop/preparation controls. The larger live video preview refreshes on mask/face-size/face-scale edits even if AirPlay sends no new frame: one original input is retained by reference and reprocessed on demand. Output is never used as input. Edits coalesce into the existing one-frame queue, identical settings do not trigger work, and disconnect/stop/failure release the retained frame.

Face alignment comparison (CUDA GPU-resident path): FaceFusion-style (5 alignment points) is the default. It uses an ArcFace128 target template with OpenCV RANSAC partial-affine estimation (100-pixel reprojection threshold), plus replicated image borders in the GPU bilinear crop. With the landmarker Off, the original five detector points are retained. With HRFFA selected, five refined points are derived from its 68-point target prediction. Source identity, pixel boost and mask semantics are unchanged. This is a crop-alignment experiment, not full FaceFusion output parity; GPU interpolation differs slightly from OpenCV warpAffine tables. It is saved and changes live, including paused-image refresh, with no model rebuild. Selecting HRFFA adds its own inference. GPEN keeps its existing ArcFace crop convention. Reference: https://github.com/facefusion/facefusion/blob/master/facefusion/face_helper.py

HRFFA - 68 points is the default landmarker on the GPU-resident CUDA path. Download it from Model management, or select Off in Face detection to use the five points from the detector. The official FaceFusion-hosted 35.8 MB model is pinned and SHA-256 verified (MIT according to publisher metadata); weights stay out of release ZIPs. The original ONNX stays in swappers, the primary 68x2 output graph in prepared, and per-level TensorRT engines in tensorrt-engines. It follows the shared TensorRT level 3/5, with CUDA fallback on build failure. Changing the model requires stopping faceswap.

Each target face is cropped from the original GPU image to RGB NCHW 256x256, normalized to [-1,1], using the FaceFusion HRFFA bounding-box convention (1.7x square). Eyes are averaged from points 36:42 and 42:48; point 30 is the nose and 48/54 the mouth corners. The refined five points feed swapping, region/occlusion crops and restoration. This first experiment does not recalculate source-photo identity. HRFFA has no calibrated confidence score in this integration; nonfinite or geometrically implausible predictions fall back to detector points. Blank frames clear old points.

Only 544 bytes of landmark coordinates are downloaded per detected frame, never image pixels; the existing detector metadata readback remains. GPU inference and crop IO stay persistent. Logs report landmarker backend, accepted refinement, and a separate landmark stage. Cyan 68-point diagnostics follow accepted predictions in the existing mask preview.

Mask expansion (0-12%, default 0) dilates the geometric/BiSeNet face mask within the existing model crop, independently of Face scale/Face size. Start around 3-5% when original cheeks/jaw remain visible. Two separable CUDA maximum-filter passes preserve the edge softness profile and leave generated facial pixels in place. It updates live, including still images, and requires no ONNX/TensorRT rebuild. At zero the previous mask composition is preserved.

Expansion occurs after combining geometry, BiSeNet and XSeg, so XSeg cannot silently recut the expanded outer contour. Excluded BiSeNet eyes/mouth regions are then reapplied. Interior occlusions remain, but their boundary is also relaxed; high expansion can uncover small objects or object edges. GPEN uses the expanded mask too. The preview shows the actual effective mask after eye/mouth protections. Expansion cannot synthesize pixels outside the swapper crop. Logs contain the expansion value and a separate mask-composition stage.

The calibrated Source geometry guidance experiment has been removed from the
active pipeline and settings page. Old geometry-intensity/reset fields are
accepted only for configuration and wire compatibility; they cannot activate a
neutral-reference warp. HRFFA alignment and direct LivePortrait pose tracking
remain available without calibration.
