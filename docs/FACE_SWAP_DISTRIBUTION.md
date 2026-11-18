# Face swap engine

The optional face-swap feature uses a local Python worker. Keep the `engine`
folder: its scripts and Python preparation helper are required at runtime.
Screen mirroring itself does not require Python or face-swap models.

On a fresh machine, select the processing backend in **Faceswap Settings**,
then click **Prepare Python Environment** under the engine status. This
downloads a pinned official Python 3.11 x64 NuGet package, verifies its SHA-256,
and installs the selected backend's dependencies in an application-local
environment. No system Python installation or administrator access is needed
when the extracted application folder is writable. Internet access is required;
NVIDIA with TensorRT can download several GB. GPU drivers remain a prerequisite.

The interface remains responsive and shows the current preparation stage.
Cancel stops the installer and its children. Failures provide a preparation log.
Successful preparation selects and saves the interpreter path automatically;
retry the engine after installing the required models. Existing Python
environments are preserved; retries use a separate directory if one already exists.

```text
face_swap/
  README.md
  engine/               Python modules, model manifest, dependency lists, optional CUDA bridge
  python/               Optional provisioned NVIDIA CUDA Python environment
  python-directml/      Optional provisioned DirectML Python environment
  python-cpu/           Optional provisioned CPU Python environment
  downloads/            Verified Python archive and package download cache
  swappers/             Original detector, identity, swapper and GPEN ONNX weights
    prepared/           Derived ONNX graphs
    tensorrt-engines/   Local GPU-specific TensorRT engines
    cuda-kernels/       Local compiled CUDA kernels
  logs/                 Local diagnostics
```

The GitHub ZIP includes the engine code, Python preparation helper and instructions.
Python is installed on demand; model weights are installed separately. No source photo,
saved personal paths, logs, compiled GPU caches, or developer environment is
included. TensorRT engines are built for the current model, GPU and runtime.

Provision a Python environment at `face_swap/python/python.exe`, or select its
executable in **Faceswap Settings > Engine & diagnostics**. Install
`engine/requirements-face-swap.txt`; NVIDIA GPU texture processing additionally
uses `requirements-face-swap-gpu.txt` and the optional
`engine/airplay-cuda-bridge.dll`. TensorRT also uses
`requirements-face-swap-tensorrt.txt`. DirectML requires a separate environment
and `requirements-face-swap-directml.txt` (do not mix its ONNX Runtime package
with the CUDA package).

Place `det_10g.onnx`, `w600k_r50.onnx`, `inswapper_128.onnx` (or its `.fp16.onnx`
variant), and optional GPEN models in `swappers`. Additional supported swappers can be downloaded from **Faceswap Settings**
after reviewing their publisher terms, or
can be installed with `python engine/download_face_swap_models.py --help` from
this folder; its default destination is the adjacent `swappers` folder.
Check the publishers' model terms before redistributing weights.

The experimental **Reference animation - LivePortrait** engine downloads four
additional ONNX models (about 338 MB) through its own Download/Verify button.
It requires the NVIDIA CUDA video bridge and GPU video decoding; CPU/DirectML
pixel transport is not supported for this mode. It uses the face detector but
does not require ArcFace, a swapper or GPEN. See `docs/FACE_SWAP.md` in the
release for direct tracking and animation controls. Its weights and compiled
engines are also excluded from the ZIP.

The CUDA bridge is included when the build has the CUDA Toolkit available.
Without the bridge, CUDA inference can still use the CPU pixel transport path.
The initial ZIP does not include Python or model weights. Python preparation
installs the runtime after extraction; model downloads remain separate.

After selecting a source photo, enabling face swap prepares models before
screen mirroring starts. Disconnecting mirroring preserves the worker;
Cancel preparation, Stop faceswap and closing the app stop it.

See `docs/FACE_SWAP.md` in the release for full setup and backend details.

Optional XSeg 1 occlusion protection can be provisioned using Download / Verify
XSeg 1 in Processing device. It is disabled by default, NVIDIA GPU-resident only;
its weights and per-machine caches are not included in the ZIP.

BiSeNet ResNet18 region masking is another optional NVIDIA GPU-resident setting,
off by default. Its Download / Verify BiSeNet button installs the publisher's
MIT-labelled export. Original eyes/mouth presets keep those detected target
regions; they do not provide eyelid tracking. Model weights are not in the ZIP.

GPU-resident CUDA also offers Box/Ellipse blending masks, four inset percentages,
edge softness and a mask/landmark preview. These use geometric GPU masks and add
no model downloads. The default ellipse remains unchanged.

Face detection also offers YOLOFace 8n, RetinaFace 10G and YuNet beside the
default SCRFD. **Model management** presents the publisher terms and verifies
weights from InsightFace for SCRFD and FaceFusion for the other detectors. Alternate weights and
GPU engine caches are not included in the ZIP. CPU/DirectML use the selected
detector as well; live CUDA processing keeps only its detection metadata on CPU.

Geometric mask controls are adjustable live without restarting the worker.
Use **Preview Settings** next to **Enable faceswap** to open live video in a
separate window while tuning. Model, detector,
execution-provider and device changes still require stopping face swap.

Face scale (80-120%, default 100%) adjusts generated facial pixels around the aligned crop centre inside the unchanged target mask. It is saved and updated live on the GPU-resident CUDA path, without changing model inputs, TensorRT engines or target-aligned XSeg/BiSeNet masks. GPEN restores the already scaled result and does not apply the zoom again. Start around 102?105%; this also moves eyes and mouth and cannot correct every alignment or expression mismatch.

Face size (global) (80-120%, default 100%) resizes the entire composited face and its effective geometric/XSeg/BiSeNet mask together about the aligned crop centre. Face scale remains a separate zoom of generated pixels within that mask; both controls combine and update live on CUDA. GPEN samples the globally resized region and restores it at that same placement, so global size is applied once. Mask diagnostics follow the resized placement; detected alignment points still indicate the original target landmarks.

The Faceswap engine card contains status and start/stop/preparation controls. The larger live video preview refreshes on mask/face-size/face-scale edits even if AirPlay sends no new frame: one original input is retained by reference and reprocessed on demand. Output is never used as input. Edits coalesce into the existing one-frame queue, identical settings do not trigger work, and disconnect/stop/failure release the retained frame.

The TensorRT compilation level (default 5, optional 3) is shared by all active TensorRT models, including the face detector, swapper, XSeg, BiSeNet, HRFFA and GPEN. Each model/level keeps a separate engine cache. Existing level-3 caches remain intact and are not reused for level 5. Failed builds fall back to CUDA with a log message, without silently compiling a lower-level TensorRT engine.

Face alignment comparison (CUDA GPU-resident path): FaceFusion-style (5 alignment points) is the default. It uses an ArcFace128 target template with OpenCV RANSAC partial-affine estimation (100-pixel reprojection threshold), plus replicated image borders in the GPU bilinear crop. With the landmarker Off, the original five detector points are retained. With HRFFA selected, five refined points are derived from its 68-point target prediction. Source identity, pixel boost and mask semantics are unchanged. This is a crop-alignment experiment, not full FaceFusion output parity; GPU interpolation differs slightly from OpenCV warpAffine tables. It is saved and changes live, including paused-image refresh, with no model rebuild. Selecting HRFFA adds its own inference. GPEN keeps its existing ArcFace crop convention. Reference: https://github.com/facefusion/facefusion/blob/master/facefusion/face_helper.py

HRFFA - 68 points is the default landmarker on the GPU-resident CUDA path. Download it from Model management, or select Off in Face detection to use the five points from the detector. The official FaceFusion-hosted 35.8 MB model is pinned and SHA-256 verified (MIT according to publisher metadata); weights stay out of release ZIPs. The original ONNX stays in swappers, the primary 68x2 output graph in prepared, and per-level TensorRT engines in tensorrt-engines. It follows the shared TensorRT level 3/5, with CUDA fallback on build failure. Changing the model requires stopping faceswap.

Each target face is cropped from the original GPU image to RGB NCHW 256x256, normalized to [-1,1], using the FaceFusion HRFFA bounding-box convention (1.7x square). Eyes are averaged from points 36:42 and 42:48; point 30 is the nose and 48/54 the mouth corners. The refined five points feed swapping, region/occlusion crops and restoration. This first experiment does not recalculate source-photo identity. HRFFA has no calibrated confidence score in this integration; nonfinite or geometrically implausible predictions fall back to detector points. Blank frames clear old points.

Only 544 bytes of landmark coordinates are downloaded per detected frame, never image pixels; the existing detector metadata readback remains. GPU inference and crop IO stay persistent. Logs report landmarker backend, accepted refinement, and a separate landmark stage. Cyan 68-point diagnostics follow accepted predictions in the existing mask preview.

Mask expansion (0-12%, default 0) dilates the geometric/BiSeNet face mask within the existing model crop, independently of Face scale/Face size. Start around 3-5% when original cheeks/jaw remain visible. Two separable CUDA maximum-filter passes preserve the edge softness profile and leave generated facial pixels in place. It updates live, including still images, and requires no ONNX/TensorRT rebuild. At zero the previous mask composition is preserved.

Expansion occurs after combining geometry, BiSeNet and XSeg, so XSeg cannot silently recut the expanded outer contour. Excluded BiSeNet eyes/mouth regions are then reapplied. Interior occlusions remain, but their boundary is also relaxed; high expansion can uncover small objects or object edges. GPEN uses the expanded mask too. The preview shows the actual effective mask after eye/mouth protections. Expansion cannot synthesize pixels outside the swapper crop. Logs contain the expansion value and a separate mask-composition stage.

Calibration and the older Source geometry guidance experiment have been removed
from the settings page and active pipeline. LivePortrait uses current HRFFA pose
and current neural expression without capturing a neutral frame. Legacy
geometry-intensity/reset settings remain wire-compatible but do not activate
that experiment. No calibration is needed after starting or reconnecting.
