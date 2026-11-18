"""Paired GPU fixture benchmark: optional XSeg, includes all processing, excludes AirPlay/display."""
import argparse
import json
from pathlib import Path
import statistics
from unittest.mock import patch
import time

from face_swap_worker import CudaEngine
import face_swap_gpu_worker as gpu


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--models',type=Path,default=Path('build/Release/face_swap/swappers'))
    p.add_argument('--source',type=Path,default=Path('build/swapper-models/source-fixture.png'))
    p.add_argument('--target',type=Path)
    p.add_argument('--frames',type=int,default=100)
    p.add_argument('--output',type=Path,default=Path('build/xseg-benchmark'))
    p.add_argument('--parser-mode',type=int,choices=(0,1,2,3,4),default=0)
    p.add_argument('--no-xseg',action='store_true')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    root=args.models.resolve()
    base=CudaEngine(root/'det_10g.onnx',root/'w600k_r50.onnx',root/'hyperswap_1b_256.onnx',0,
                    swap_model='hyperswap_1b_256',swapper_backend='tensorrt_fp32',swapper_builder=5)
    base.set_source(args.source)
    cv2,np=base.cv2,base.np
    photo=cv2.imread(str(args.target or args.source));h,w=photo.shape[:2]
    w,h=min(w,1280)//2*2,min(h,1080)//2*2
    photo=cv2.resize(photo,(w,h))
    cp=gpu.load_cupy(root/'cuda-kernels')
    fixture=cp.asarray(cv2.cvtColor(photo,cv2.COLOR_BGR2BGRA));cp.cuda.get_current_stream().synchronize()
    class Bridge:
        def __init__(self,*args):self.device=0
        def read(self,image,stream):image[...] = fixture
        def write(self,image,stream):stream.synchronize()
        def close(self):pass
    started=time.perf_counter()
    with patch.object(gpu,'TextureBridge',Bridge):
        engine=gpu.GpuImageEngine(base,None,(0,0,0),w,h,root/'cuda-kernels',occluder_path=None if args.no_xseg else root/'xseg_1.onnx',
            parser_path=root/'bisenet_resnet_18.onnx' if args.parser_mode else None,parser_mode=args.parser_mode)
    mask=engine.occluder;parser=engine.parser
    active=parser or mask
    name="masks" if parser else "xseg"
    results={'scope':'GPU-held still fixture, full processing; excludes AirPlay, D3D11, sockets and presentation',
             'backend':active.backend,'parser_mode':args.parser_mode,'xseg':bool(mask),'prepare_seconds':time.perf_counter()-started,'size':[w,h]}
    timings={False:[],True:[]}
    for i in range(args.frames+10):
        for enabled in ((False,True) if i%2 else (True,False)):
            engine.occluder=mask if enabled else None
            engine.parser=parser if enabled else None
            elapsed=engine.process()
            if engine.last_face_count<1:raise RuntimeError('No face in benchmark fixture')
            if i>=10:timings[enabled].append(elapsed)
    for enabled,values in timings.items():
        results[name if enabled else 'off']={'median_ms':statistics.median(values),'p95_ms':float(np.percentile(values,95))}
        engine.occluder=mask if enabled else None
        engine.parser=parser if enabled else None
        engine.process();cv2.imwrite(str(args.output/('xseg.png' if enabled else 'off.png')),cp.asnumpy(engine.image))
    # Diagnostic mask readback is outside timing and absent from production.
    cv2.imwrite(str(args.output/'mask.png'),cp.asnumpy(cp.clip((active.blurred-.5)*2,0,1))*255)
    results['overhead_ms']=results[name]['median_ms']-results['off']['median_ms']
    print(json.dumps(results,indent=2),flush=True)
    (args.output/'results.json').write_text(json.dumps(results,indent=2))
    engine.occluder=mask;engine.parser=parser;engine.close()

if __name__=='__main__':main()
