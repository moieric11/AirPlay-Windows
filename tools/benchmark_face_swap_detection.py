"""Compare original and fused detection on identical GPU-held photo fixtures.

Runs HyperSwap A / TensorRT with alternating reference/optimized execution order.
Includes detection, swap and paste; excludes AirPlay, D3D11 and display.
Checks exact pixels, landmarks and face counts on multi-face, empty, moving
and resized inputs. Set AIRPLAY_FACE_SWAP_NVTX=1 for labeled Nsight ranges.
"""
import sys,json,statistics,argparse
from pathlib import Path
from types import MethodType
from unittest.mock import patch
from face_swap_worker import CudaEngine
import face_swap_gpu_worker as gpu

def reference_detect(self):
    cp, np = self.cp, self.np
    w, h = self.width, self.height
    ratio = h / w
    rw, rh = (max(1, int(320 / ratio)), 320) if ratio > 1 else (320, max(1, int(320 * ratio)))
    self.detector_pack(((320 * 320 + 255) // 256,), (256,),
        (self.image, np.int32(w), np.int32(h), self.detector.inputs[0], np.int32(320), np.int32(rw), np.int32(rh)))
    values = self.detector.run()
    fmc = len(self.centers)
    candidates = []
    for index, stride in enumerate(self.base.detector._feat_stride_fpn):
        scores = values[index].reshape(-1)
        selected = cp.flatnonzero(scores >= .5)
        if selected.size == 0:
            continue
        centers = self.centers[index][selected]
        distances = values[index + fmc].reshape(-1, 4)[selected] * stride
        boxes = cp.concatenate((centers - distances[:, :2], centers + distances[:, 2:]), axis=1)
        points = values[index + 2 * fmc].reshape(-1, 5, 2)[selected] * stride + centers[:, None, :]
        # Detection outputs only: boxes, scores and ten landmark values.
        packed = cp.concatenate((boxes, scores[selected, None], points.reshape(-1, 10)), axis=1)
        host = cp.asnumpy(packed)
        self.metadata_bytes += host.nbytes
        candidates.append(host)
    if not candidates:
        self.last_face_count = 0
        return None
    candidates = np.concatenate(candidates)
    candidates[:, :4] /= rh / h
    candidates[:, 5:] /= rh / h
    candidates = candidates[np.argsort(candidates[:, 4])[::-1]]
    keep = self.base.detector.nms(candidates[:, :5])
    candidates = candidates[keep]
    self.last_face_count = len(candidates)
    areas = (candidates[:, 2] - candidates[:, 0]) * (candidates[:, 3] - candidates[:, 1])
    return candidates[np.argmax(areas), 5:].reshape(5, 2)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--target',type=Path,required=True)
    parser.add_argument('--swapper',type=Path,default=Path('build/Release/face_swap/swappers/hyperswap_1a_256.onnx'))
    parser.add_argument('--frames',type=int,default=60)
    parser.add_argument('--output',type=Path,default=Path('build/detection-optimization'))
    args=parser.parse_args()
    if args.frames<4:parser.error('Use at least four paired measured frames')
    r=args.models;out=args.output;out.mkdir(parents=True,exist_ok=True)
    base=CudaEngine(r/'det_10g.onnx',r/'w600k_r50.onnx',args.swapper,0,swap_model='hyperswap_1a_256',swapper_backend='tensorrt_fp32')
    base.set_source(args.source.resolve())
    np,cv2=base.np,base.cv2;cp=gpu.load_cupy(Path('build/cupy-cache').resolve())
    photos={'multi':cv2.imread(str(args.target)),'portrait':cv2.imread(str(args.source))}
    banks={}
    for dims,photo in [((1280,720),photos['multi']),((498,1080),photos['portrait'])]:
     w,h=dims;scale=min(w/photo.shape[1],h/photo.shape[0]);resized=cv2.resize(photo,(int(photo.shape[1]*scale),int(photo.shape[0]*scale)))
     image=np.zeros((h,w,3),np.uint8);y=(h-resized.shape[0])//2;x=(w-resized.shape[1])//2;image[y:y+resized.shape[0],x:x+resized.shape[1]]=resized
     shifted=cv2.warpAffine(image,np.float32([[1,0,8],[0,1,4]]),(w,h))
     banks[dims]=[cp.asarray(cv2.cvtColor(v,cv2.COLOR_BGR2BGRA)) for v in (image,shifted,np.zeros_like(image))]
    cp.cuda.get_current_stream().synchronize()
    class Bridge:
     def __init__(self,dll,ticket,w,h):self.device=0;self.dims=(w,h);self.index=0
     def read(self,array,stream):array[:]=banks[self.dims][self.index]
     def write(self,array,stream):stream.synchronize()
     def close(self):pass
    with patch.object(gpu,'TextureBridge',Bridge):
     e=gpu.GpuImageEngine(base,None,(0,0,0),1280,720,Path('build/cupy-cache').resolve(),detector_tensorrt=False)
     assert e.swap_execution=='TensorRT FP32'
     with e.stream:
      e.centers=[]
      for stride in base.detector._feat_stride_fpn:
       centers=cp.stack(cp.meshgrid(cp.arange(320//stride,dtype=cp.float32),cp.arange(320//stride,dtype=cp.float32)),axis=-1).reshape(-1,2)
       e.centers.append(cp.repeat(centers,base.detector._num_anchors,axis=0)*stride)
     methods={'reference':MethodType(reference_detect,e),'optimized':MethodType(gpu.GpuImageEngine.detect,e)}
     results={}
     for dims in banks:
      w,h=dims
      if (e.width,e.height)!=dims:assert e.replace_textures((0,0,0),w,h)
      # Pixel, face count and landmark equivalence with changing and empty inputs.
      correctness=[]
      for fixture in (0,1,2,0):
       checks=[]
       for name,method in methods.items():
        e.detect=method;e.bridge.index=fixture;e.process();count=e.last_face_count;image=cp.asnumpy(e.image)
        with e.stream:landmarks=e.detect()
        checks.append((image,count,landmarks.copy() if landmarks is not None else None))
       np.testing.assert_array_equal(checks[0][0],checks[1][0]);assert checks[0][1]==checks[1][1]
       if checks[0][2] is not None:np.testing.assert_array_equal(checks[0][2],checks[1][2])
       else:assert checks[1][2] is None
       assert checks[0][1]>0 if fixture!=2 else checks[0][1]==0
       correctness.append({'fixture':fixture,'faces':checks[0][1],'pixel_max_delta':0,'landmarks_identical':True})
      for _ in range(6):
       for name,method in methods.items():e.detect=method;e.bridge.index=0;e.process()
      timings={k:[] for k in methods};stages={k:[] for k in methods};byte_counts={}
      for index in range(args.frames):
       for name in (('reference','optimized') if index%2==0 else ('optimized','reference')):
        e.detect=methods[name];e.bridge.index=index%2
        with e.profile_range('Detection/'+name):
         timings[name].append(e.process())
        stages[name].append(e.last_timings['detect']);byte_counts[name]=e.metadata_bytes
      row={'width':w,'height':h,'frames_each':args.frames,'correctness':correctness}
      for name in methods:row[name]={'frame_median_ms':statistics.median(timings[name]),'frame_p95_ms':float(np.quantile(timings[name],.95)),'detect_median_ms':statistics.median(stages[name]),'detect_p95_ms':float(np.quantile(stages[name],.95)),'metadata_bytes':byte_counts[name]}
      row['speedup']=row['reference']['frame_median_ms']/row['optimized']['frame_median_ms'];results[str(dims)]=row;print(json.dumps(row),flush=True)
      (out/'results.json').write_text(json.dumps(results,indent=2))
     e.close()


if __name__=='__main__':
    main()
