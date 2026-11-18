"""Geometric blending masks and optional GPU diagnostic overlay."""
import numpy as np


def normalize_settings(config):
    result={'shape':config.get('shape','ellipse'),'padding':tuple(config.get('padding',(0,0,0,0))),
            'blur':config.get('blur',18),'preview':config.get('preview',0),'face_scale':config.get('face_scale',100),'face_size':config.get('face_size',100),'alignment':config.get('alignment',0),'expansion':config.get('expansion',0),'shape_transfer':config.get('shape_transfer',0),'shape_reset':config.get('shape_reset',0),'animation_strength':config.get('animation_strength',100)}
    if result['shape'] not in ('ellipse','box'):raise ValueError('Expected ellipse or box')
    if len(result['padding'])!=4 or any(type(v) is not int or not 0<=v<=45 for v in result['padding']):
        raise ValueError('Mask padding must contain top/right/bottom/left percentages in 0..45')
    if type(result['blur']) is not int or not 0<=result['blur']<=50:raise ValueError('Mask blur must be 0..50 percent')
    if type(result['preview']) is not int or result['preview'] not in (0,1,2):raise ValueError('Invalid mask preview')
    if type(result['face_scale']) is not int or not 80<=result['face_scale']<=120:raise ValueError('Face scale must be 80..120 percent')
    if type(result['face_size']) is not int or not 80<=result['face_size']<=120:raise ValueError('Face size must be 80..120 percent')
    if type(result['alignment']) is not int or result['alignment'] not in (0,1):raise ValueError('Alignment must be 0 or 1')
    if type(result['expansion']) is not int or not 0<=result['expansion']<=12:raise ValueError('Mask expansion must be 0..12 percent')
    if type(result['shape_transfer']) is not int or not 0<=result['shape_transfer']<=100:raise ValueError('Shape transfer must be 0..100')
    if type(result['shape_reset']) is not int or not 0<=result['shape_reset']<=2147483647:raise ValueError('Invalid shape calibration generation')
    if type(result['animation_strength']) is not int or not 0<=result['animation_strength']<=100:raise ValueError('Animation strength must be 0..100')
    return result


def resize_face_placement(matrix, side, face_size=100):
    """Output-to-crop transform: scale face and mask about the aligned crop centre."""
    if type(face_size) is not int or not 80<=face_size<=120:
        raise ValueError('Face size must be 80..120 percent')
    if face_size==100:return matrix
    factor=face_size/100
    result=matrix.copy()
    center=(side-1)*.5
    result[:,:2]/=factor
    result[:,2]=center+(matrix[:,2]-center)/factor
    return result


def geometric_mask(side,shape='ellipse',padding=(0,0,0,0),blur=18):
    if shape not in ('ellipse','box') or not isinstance(side,int) or side<2:
        raise ValueError('Expected ellipse/box and a positive mask size')
    if len(padding)!=4 or any(type(v) is not int or not 0<=v<=45 for v in padding):
        raise ValueError('Mask padding must contain top/right/bottom/left percentages in 0..45')
    if type(blur) is not int or not 0<=blur<=50:raise ValueError('Mask blur must be 0..50 percent')
    top,right,bottom,left=[side*v/100 for v in padding]
    width,height=side-left-right,side-top-bottom
    y,x=np.mgrid[:side,:side].astype(np.float32)
    if shape=='ellipse':
        radius=np.sqrt(((x-(left+(width-1)/2))/(width*.46))**2+
                       ((y-(top+(height-1)/2))/(height*.49))**2)
        alpha=np.clip((1-radius)/(blur/100),0,1) if blur else (radius<=1).astype(np.float32)
    else:
        distance=np.minimum(np.minimum(x-left,side-1-right-x),np.minimum(y-top,side-1-bottom-y))
        edge=min(width,height)*.5*blur/100
        alpha=np.clip(distance/edge,0,1) if blur else (distance>=0).astype(np.float32)
    return (alpha*alpha*(3-2*alpha)).astype(np.float32)


KERNELS=r'''
extern "C" __global__ void expand_mask(const float* src,float* dst,int side,int radius,int vertical) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=side*side)return;
 int x=k%side,y=k/side;float value=0.f;
 for(int offset=-radius;offset<=radius;++offset) {
  int xx=x+(vertical?0:offset),yy=y+(vertical?offset:0);
  if(xx>=0&&yy>=0&&xx<side&&yy<side)value=fmaxf(value,src[yy*side+xx]);
 }
 dst[k]=value;
}
__device__ float mask_at(const float* mask,int side,float x,float y) {
 if(x<0 || y<0 || x>side-1 || y>side-1)return 0.f;
 int ix=(int)x,iy=(int)y,jx=min(ix+1,side-1),jy=min(iy+1,side-1);float dx=x-ix,dy=y-iy;
 return (1-dy)*((1-dx)*mask[iy*side+ix]+dx*mask[iy*side+jx])+dy*((1-dx)*mask[jy*side+ix]+dx*mask[jy*side+jx]);
}
extern "C" __global__ void show_mask(unsigned char* image,int w,int h,const float* mask,int side,
 float a,float b,float c,float d,float e,float f,int mode,int present) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=w*h)return;
 int x=k%w,y=k/w;
 float alpha=present ? fminf(1.f,fmaxf(0.f,mask_at(mask,side,a*x+b*y+c,d*x+e*y+f))) : 0.f;
 if(mode==2) {unsigned char value=(unsigned char)(alpha*255+.5f);for(int ch=0;ch<3;++ch)image[k*4+ch]=value;}
 else {float strength=alpha*.35f;for(int ch=0;ch<3;++ch){float target=ch==1?255.f:0.f;image[k*4+ch]=(unsigned char)(image[k*4+ch]*(1-strength)+target*strength+.5f);}}
 image[k*4+3]=255;
}
extern "C" __global__ void show_points(unsigned char* image,int w,int h,
 float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3,float x4,float y4) {
 int k=blockIdx.x*blockDim.x+threadIdx.x;if(k>=405)return;
 int point=k/81,dx=k%9-4,dy=(k%81)/9-4;if(dx*dx+dy*dy>16)return;
 float xs[5]={x0,x1,x2,x3,x4},ys[5]={y0,y1,y2,y3,y4};
 int x=(int)roundf(xs[point])+dx,y=(int)roundf(ys[point])+dy;
 if(x<0 || y<0 || x>=w || y>=h)return;
 image[(y*w+x)*4]=0;image[(y*w+x)*4+1]=65;image[(y*w+x)*4+2]=255;image[(y*w+x)*4+3]=255;
}
'''


class MaskExpansion:
    """Two separable GPU dilation passes; pixels and facial features do not move."""
    def __init__(self,cp,sides):
        self.kernel=cp.RawModule(code=KERNELS).get_function('expand_mask')
        self.buffers={side:(cp.empty((side,side),cp.float32),cp.empty((side,side),cp.float32))
                      for side in set(sides) if side}

    def apply(self,mask,side,percent):
        if type(percent) is not int or not 0<=percent<=12:raise ValueError('Mask expansion must be 0..12 percent')
        if percent==0:return mask
        radius=max(1,round(side*percent/100))
        temporary,result=self.buffers[side]
        grid=((side*side+255)//256,)
        self.kernel(grid,(256,),(mask,temporary,np.int32(side),np.int32(radius),np.int32(0)))
        self.kernel(grid,(256,),(temporary,result,np.int32(side),np.int32(radius),np.int32(1)))
        return result


class MaskPreview:
    def __init__(self,cp,mode):
        if mode not in (1,2):raise ValueError('Mask preview must be overlay or mask only')
        self.mode=mode;self.module=cp.RawModule(code=KERNELS)
        self.overlay=self.module.get_function('show_mask');self.points=self.module.get_function('show_points')

    def apply(self,image,mask,side,matrix,landmarks):
        h,w=image.shape[:2]
        if matrix is None:matrix=np.zeros((2,3),np.float32)
        self.overlay(((w*h+255)//256,),(256,),(image,np.int32(w),np.int32(h),mask,np.int32(side),
            *(np.float32(v) for v in matrix.ravel()),np.int32(self.mode),np.int32(landmarks is not None)))
        if landmarks is not None:
            self.points((2,),(256,),(image,np.int32(w),np.int32(h),*(np.float32(v) for v in landmarks.ravel())))
