"""Fixed SCRFD metadata buffers: one decode kernel and one host transfer.

All anchors retain their original order, including equal-score candidates.
The bounded transfer contains detection metadata only, never image pixels.
"""
DECODE = r'''
extern "C" __global__ void decode_detection(
    const unsigned long long* pointers, const int* offsets, const int* strides,
    int heads, int anchors, int total, float* out) {
    int index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index>=total)return;
    int head=0;
    while(head+1<heads && index>=offsets[head+1])++head;
    int local=index-offsets[head], stride=strides[head], width=320/stride;
    const float* scores=reinterpret_cast<const float*>(pointers[head]);
    float score=scores[local];
    float* row=out+index*15;
    row[4]=score;
    // Always replace the score, so no candidate can leak from a prior frame.
    if(!(score>=0.5f))return;
    const float* boxes=reinterpret_cast<const float*>(pointers[head+heads])+local*4;
    const float* points=reinterpret_cast<const float*>(pointers[head+2*heads])+local*10;
    float x=(local/anchors%width)*stride, y=(local/anchors/width)*stride;
    row[0]=x-boxes[0]*stride; row[1]=y-boxes[1]*stride;
    row[2]=x+boxes[2]*stride; row[3]=y+boxes[3]*stride;
    for(int i=0;i<5;++i) {
        row[5+2*i]=points[2*i]*stride+x;
        row[6+2*i]=points[2*i+1]*stride+y;
    }
}
'''


class DetectionMetadata:
    def __init__(self, cp, outputs, strides, anchors):
        import numpy as np
        self.cp = cp
        self.heads, self.anchors = len(strides), anchors
        counts = [(320 // stride)**2 * anchors for stride in strides]
        if not counts or anchors < 1 or len(outputs) != 3*len(strides):
            raise ValueError("Unsupported SCRFD output layout")
        for components, group in ((1, 0), (4, 1), (10, 2)):
            for index, count in enumerate(counts):
                value = outputs[group*self.heads+index]
                if value.dtype != cp.float32 or value.size != count*components or not value.flags.c_contiguous:
                    raise ValueError("SCRFD metadata requires contiguous fixed float32 outputs")
        self.total = sum(counts)
        self.pointers = cp.asarray([value.data.ptr for value in outputs], dtype=cp.uint64)
        self.offsets = cp.asarray(np.cumsum([0]+counts), dtype=cp.int32)
        self.strides = cp.asarray(strides, dtype=cp.int32)
        self.device = cp.empty((self.total, 15), dtype=cp.float32)
        self.pinned = cp.cuda.alloc_pinned_memory(self.device.nbytes)
        self.host = np.frombuffer(self.pinned, dtype=np.float32, count=self.total*15).reshape(self.total, 15)
        self.kernel = cp.RawKernel(DECODE, "decode_detection", options=("--std=c++11", "--fmad=false"))

    def download(self, stream):
        import numpy as np
        # Outputs are persistent ORT-bound arrays. There is no variable-size
        # GPU allocation, anchor-count readback or candidate truncation here.
        self.kernel(((self.total+255)//256,), (256,),
                    (self.pointers, self.offsets, self.strides, np.int32(self.heads),
                     np.int32(self.anchors), np.int32(self.total), self.device), stream=stream)
        self.device.get(stream=stream, out=self.host, blocking=True)
        return self.host[self.host[:, 4] >= .5]
