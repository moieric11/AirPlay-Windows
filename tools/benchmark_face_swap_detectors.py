from pathlib import Path
import cv2,json,time
from unittest.mock import patch
from face_swap_worker import CudaEngine
import face_swap_gpu_worker as gpu

root=Path('build/Release/face_swap/swappers').resolve()
output=Path('build/detector-comparison');output.mkdir(exist_ok=True)
source=Path('build/swapper-models/source-fixture.png')
photo=cv2.imread(str(source));h,w=photo.shape[:2]
cp=gpu.load_cupy(root/'cuda-kernels');fixture=cp.asarray(cv2.cvtColor(photo,cv2.COLOR_BGR2BGRA));cp.cuda.get_current_stream().synchronize()
class Bridge:
 def __init__(self,*args):self.device=0
 def read(self,image,stream):image[...] = fixture
 def write(self,image,stream):stream.synchronize()
 def close(self):pass
results={}
for kind,name in [('scrfd','det_10g'),('yoloface','yoloface_8n'),('retinaface','retinaface_10g'),('yunet','yunet_2023_mar')]:
 print('Preparing '+kind,flush=True)
 start=time.perf_counter()
 base=CudaEngine(root/(name+'.onnx'),root/'w600k_r50.onnx',root/'hyperswap_1b_256.onnx',0,
     swap_model='hyperswap_1b_256',swapper_backend='tensorrt_fp32',detector_kind=kind,model_cache=root/'prepared')
 base.set_source(source)
 with patch.object(gpu,'TextureBridge',Bridge):engine=gpu.GpuImageEngine(base,None,(0,0,0),w,h,root/'cuda-kernels')
 prep=time.perf_counter()-start
 total=[];detect=[]
 for i in range(90):
  elapsed=engine.process()
  if engine.last_face_count<1:raise RuntimeError('No face detected: '+kind)
  if i>=10:total.append(elapsed);detect.append(engine.last_timings['detect'])
 cv2.imwrite(str(output/(kind+'.png')),cp.asnumpy(engine.image))
 points=base.faces(photo)[0].kps.tolist()
 import statistics,numpy as np
 results[kind]={'backend':engine.detector_backend,'side':320 if kind in ('scrfd','retinaface') else 640,
   'prepare_s':prep,'total_median_ms':statistics.median(total),'detect_median_ms':statistics.median(detect),
   'total_p95_ms':float(np.percentile(total,95)),'faces':engine.last_face_count,'source_points':points}
 # A blank frame must not reuse detections from the preceding image.
 fixture.fill(0);cp.cuda.get_current_stream().synchronize();engine.process()
 if engine.last_face_count:raise RuntimeError('Stale detections: '+kind)
 fixture[...] = cp.asarray(cv2.cvtColor(photo,cv2.COLOR_BGR2BGRA));cp.cuda.get_current_stream().synchronize()
 print(kind,results[kind],flush=True)
 engine.close();del engine,base
 (output/'results.json').write_text(json.dumps(results,indent=2))
