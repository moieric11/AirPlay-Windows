"""Compare fixed 320px SCRFD CUDA Graph and TensorRT FP32 in the resident pipeline.

Alternating paired runs keep HyperSwap A/TensorRT and fused metadata decoding
unchanged. Validates face counts, landmarks and full-frame pixel differences
on moving, empty and resized GPU-held fixtures. Excludes AirPlay/D3D11/display.
"""
import sys,time,json,statistics,argparse
from pathlib import Path
from unittest.mock import patch
from face_swap_worker import CudaEngine
import face_swap_gpu_worker as gpu
from face_swap_detection import DetectionMetadata

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--target',type=Path,required=True)
    parser.add_argument('--frames',type=int,default=60)
    parser.add_argument('--output',type=Path,default=Path('build/detector-tensorrt'))
    args=parser.parse_args()
    if args.frames<4:parser.error('Use at least four paired measured frames')
    r=args.models;out=args.output;out.mkdir(parents=True,exist_ok=True)
    base=CudaEngine(r/'det_10g.onnx',r/'w600k_r50.onnx',Path('build/Release/face_swap/swappers/hyperswap_1a_256.onnx'),0,swap_model='hyperswap_1a_256',swapper_backend='tensorrt_fp32')
    base.set_source(args.source.resolve());np,cv2=base.np,base.cv2;cp=gpu.load_cupy(Path('build/cupy-cache').resolve())
    banks={}
    for dims,photo in [((1280,720),cv2.imread(str(args.target))),((498,1080),cv2.imread(str(args.source)))]:
     w,h=dims;scale=min(w/photo.shape[1],h/photo.shape[0]);scaled=cv2.resize(photo,(int(photo.shape[1]*scale),int(photo.shape[0]*scale)))
     image=np.zeros((h,w,3),np.uint8);x=(w-scaled.shape[1])//2;y=(h-scaled.shape[0])//2;image[y:y+scaled.shape[0],x:x+scaled.shape[1]]=scaled
     banks[dims]=[cp.asarray(cv2.cvtColor(v,cv2.COLOR_BGR2BGRA)) for v in (image,cv2.warpAffine(image,np.float32([[1,0,8],[0,1,4]]),(w,h)),np.zeros_like(image))]
    cp.cuda.get_current_stream().synchronize()
    class Bridge:
     def __init__(self,dll,ticket,w,h):self.device=0;self.dims=(w,h);self.index=0
     def read(self,array,stream):array[:]=banks[self.dims][self.index]
     def write(self,array,stream):stream.synchronize()
     def close(self):pass
    with patch.object(gpu,'TextureBridge',Bridge):
     e=gpu.GpuImageEngine(base,None,(0,0,0),1280,720,Path('build/cupy-cache').resolve(),detector_tensorrt=False)
     print('Preparing fixed SCRFD TensorRT FP32',flush=True)
     with e.stream:
      shapes=[tuple(v.shape) for v in e.detector.outputs]
      trt,backend=gpu.make_detector(cp,base._gpu_detector_model,e.stream,e.device,Path('build/cupy-cache'),shapes,True)
      assert backend=='TensorRT FP32'
      meta=DetectionMetadata(cp,trt.outputs,base.detector._feat_stride_fpn,base.detector._num_anchors)
     models={'cuda':(e.detector,e.detection_metadata),'tensorrt':(trt,meta)};e.stream.synchronize()
     results={}
     for dims in banks:
      w,h=dims
      if (e.width,e.height)!=dims:assert e.replace_textures((0,0,0),w,h)
      correctness=[]
      for fixture in (0,1,2,0):
       checks=[]
       for name,(model,metadata) in models.items():
        e.detector,e.detection_metadata=model,metadata;e.bridge.index=fixture;e.process()
        image=cp.asnumpy(e.image);count=e.last_face_count
        with e.stream:landmarks=e.detect()
        checks.append((image,count,landmarks.copy() if landmarks is not None else None))
       assert checks[0][1]==checks[1][1],(dims,fixture,[c[1] for c in checks])
       delta=np.abs(checks[0][0].astype(np.float32)-checks[1][0].astype(np.float32))
       if checks[0][2] is not None:
        landmark_error=float(np.abs(checks[0][2]-checks[1][2]).max());assert landmark_error<.1,landmark_error
       else:assert checks[1][2] is None;landmark_error=0
       assert delta.max()<=2 and delta.mean()<.05,(dims,fixture,float(delta.max()),float(delta.mean()))
       row=dict(fixture=fixture,faces=checks[0][1],max_pixel_delta=float(delta.max()),mean_pixel_delta=float(delta.mean()),max_landmark_delta=landmark_error);correctness.append(row);print(row,flush=True)
       cv2.imwrite(str(out/(str(w)+'x'+str(h)+'-'+str(fixture)+'-cuda.png')),checks[0][0]);cv2.imwrite(str(out/(str(w)+'x'+str(h)+'-'+str(fixture)+'-trt.png')),checks[1][0])
      for _ in range(6):
       for model,metadata in models.values():e.detector,e.detection_metadata=model,metadata;e.bridge.index=0;e.process()
      frames={k:[] for k in models};times={k:[] for k in models}
      for index in range(args.frames):
       for name in (('cuda','tensorrt') if index%2==0 else ('tensorrt','cuda')):
        e.detector,e.detection_metadata=models[name];e.bridge.index=index%2;frames[name].append(e.process());times[name].append(e.last_timings['detect'])
      row={'correctness':correctness}
      for name in models:row[name]=dict(frame_median_ms=statistics.median(frames[name]),detect_median_ms=statistics.median(times[name]),frame_p95_ms=float(np.quantile(frames[name],.95)))
      results[str(dims)]=row;print(json.dumps(row),flush=True);(out/'results.json').write_text(json.dumps(results,indent=2))
     e.close()
     # Exercise the application's default: a TensorRT swapper also enables
     # the compatible TensorRT detector, without a diagnostic override.
     e=gpu.GpuImageEngine(base,None,(0,0,0),498,1080,Path('build/cupy-cache').resolve())
     assert e.detector_backend=='TensorRT FP32'
     e.process();assert e.last_face_count==1
     results['automatic_selection']={'detector':e.detector_backend,'swapper':e.swap_execution,'faces':e.last_face_count}
     (out/'results.json').write_text(json.dumps(results,indent=2))
     e.close()


if __name__=='__main__':
    main()
