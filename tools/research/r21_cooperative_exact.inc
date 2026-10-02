// Research-only cooperative FP32 staging. Never enabled in the timed engine.
namespace hgnv2gpu {
template<int BT,int RW,int RowWaves,int TokenWaves,bool StageX>
__global__ __launch_bounds__(32*RowWaves*TokenWaves) void expert_cooperative(
 const uint8_t* __restrict__ q,const __half* __restrict__ scales,
 const float* __restrict__ x,float* __restrict__ y,const int* __restrict__ tokidx,
 const int* __restrict__ tiles,const int* __restrict__ ntiles,int cols,int erows) {
 if((int)blockIdx.y>=*ntiles)return;
 constexpr int RT=RW*RowWaves,TT=BT*TokenWaves,Threads=32*RowWaves*TokenWaves;
 const int* tile=tiles+blockIdx.y*3;const int start=TT*blockIdx.z;
 if(start>=tile[2])return;
 const int lane=threadIdx.x&31,wave=threadIdx.x/32;
 const int rbase=blockIdx.x*RT,wr=(wave%RowWaves)*RW,wt=(wave/RowWaves)*BT;
 const int first=tile[1]+start,live=min(TT,tile[2]-start);
 __shared__ float weights[RT*128],inputs[StageX?TT*128:1];
 const float* in[BT];float acc[RW][BT]={};
 #pragma unroll
 for(int t=0;t<BT;++t){int ix=first+min(wt+t,live-1);if(tokidx)ix=tokidx[ix];in[t]=x+(size_t)ix*cols;}
 for(int g=0;g<cols/128;++g) {
  for(int idx=threadIdx.x;idx<RT*128;idx+=Threads) {
   int row=min(rbase+idx/128,erows-1),c=g*128+idx%128;
   size_t off=((size_t)tile[0]*erows+row)*cols+c;
   weights[idx]=float(int((q[off/2]>>(4*(off%2)))&15)-8)*__half2float(scales[off/128]);
  }
  if constexpr(StageX) {
   for(int idx=threadIdx.x;idx<TT*128;idx+=Threads) {
    int ix=first+min(idx/128,live-1);if(tokidx)ix=tokidx[ix];
    inputs[idx]=x[(size_t)ix*cols+g*128+idx%128];
   }
  }
  __syncthreads();
  #pragma unroll
  for(int j=0;j<4;++j) {
   float w[RW];
   #pragma unroll
   for(int r=0;r<RW;++r)w[r]=weights[(wr+r)*128+j*32+lane];
   #pragma unroll
   for(int t=0;t<BT;++t) {
    float xv=StageX?inputs[(wt+t)*128+j*32+lane]:in[t][g*128+j*32+lane];
    #pragma unroll
    for(int r=0;r<RW;++r)acc[r][t]=fmaf(w[r],xv,acc[r][t]);
   }
  }
  __syncthreads();
 }
 #pragma unroll
 for(int r=0;r<RW;++r) {
  #pragma unroll
  for(int t=0;t<BT;++t) {
   for(int h=16;h;h/=2)acc[r][t]+=__shfl_down(acc[r][t],h,32);
   if(lane==0 && wt+t<live && rbase+wr+r<erows)y[(size_t)(first+wt+t)*erows+rbase+wr+r]=acc[r][t];
  }
 }
}
}
