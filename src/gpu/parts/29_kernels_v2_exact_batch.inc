// R20 exact FP32 expert candidates. Each accumulator retains c=lane+32*j
// and the reference shuffle reduction. Only the reuse of inputs changes.
namespace hgnv2gpu {
__global__ void compare_bf16_bits(const uint16_t* a,const uint16_t* b,size_t n,unsigned* count) {
  size_t i=(size_t)blockIdx.x*blockDim.x+threadIdx.x;
  if(i<n && a[i]!=b[i])atomicAdd(count,1u);
}
__device__ __forceinline__ void exact_fwht128(float (&v)[4],int lane) {
  #pragma unroll
  for(int h=1;h<32;h*=2){
    #pragma unroll
    for(int j=0;j<4;++j){float p=__shfl_xor(v[j],h,32);v[j]=(lane&h)?p-v[j]:v[j]+p;}
  }
  const float a=v[0]+v[1],b=v[0]-v[1],c=v[2]+v[3],d=v[2]-v[3];
  v[0]=a+c;v[1]=b+d;v[2]=a-c;v[3]=b-d;
}
// Keep the reference's column-then-row transform and scale association.
// Wave shuffles replace its 14 block barriers; arithmetic is unchanged.
__global__ __launch_bounds__(256) void ht_dense_exact_streamed(
    const uint8_t* codes,const float* su,const float* sv,uint16_t* out,int cols) {
  __shared__ float tile[128*128];
  const int tx=blockIdx.x%(cols/128),ty=blockIdx.x/(cols/128);
  const uint32_t* packed=(const uint32_t*)(codes+(size_t)blockIdx.x*8192);
  const int lane=threadIdx.x&31,wave=threadIdx.x>>5;
  for(int it=0;it<8;++it){
    int blk=it*8+wave,g=blk/8,q=blk%8;
    uint32_t cur=packed[g*256+q*32+lane],prev=__shfl(cur,(lane+31)%32,32);
    const int r=q*16+lane%16,c0=g*16+(lane/16)*8;
    #pragma unroll
    for(int j=0;j<8;++j){
      uint16_t state=uint16_t(((uint64_t(prev)<<32)|cur)>>(28-4*j));
      tile[r*128+((c0+j)^(r&31))]=cb(state);
    }
  }
  __syncthreads();
  for(int rr=0;rr<16;++rr){
    const int r=wave*16+rr;float v[4];
    #pragma unroll
    for(int j=0;j<4;++j)v[j]=tile[r*128+((j*32+lane)^(r&31))];
    exact_fwht128(v,lane);
    #pragma unroll
    for(int j=0;j<4;++j)tile[r*128+((j*32+lane)^(r&31))]=v[j];
  }
  __syncthreads();
  for(int cc=0;cc<16;++cc){
    const int c=wave*16+cc,C=tx*128+c;float v[4];
    #pragma unroll
    for(int j=0;j<4;++j){int r=j*32+lane;v[j]=tile[r*128+(c^(r&31))];}
    exact_fwht128(v,lane);
    #pragma unroll
    for(int j=0;j<4;++j){int R=ty*128+j*32+lane;out[(size_t)R*cols+C]=bf(((v[j]*(1.f/128.f))*su[C])*sv[R]);}
  }
}
template<int BT, int RW, bool StageX=false, int NW=8, int StageGroups=1, bool DirectQ=false, bool UniformRows=false, int JUnroll=4>
__global__ __launch_bounds__(NW*32) void expert_batch_exact_reuse(
    const uint8_t* __restrict__ q, const __half* __restrict__ scales,
    const float* __restrict__ x, float* __restrict__ y,
    const int* __restrict__ tokidx, const int* __restrict__ tiles,
    const int* __restrict__ ntiles, int cols, int erows) {
  if ((int)blockIdx.y >= *ntiles) return;
  const int* tile=tiles+3*blockIdx.y;
  const int start=BT*blockIdx.z;
  if(start>=tile[2])return;
  const int lane=threadIdx.x&31, wave=threadIdx.x>>5;
  int row0=(blockIdx.x*NW+wave)*RW;
  if constexpr(UniformRows) row0=__builtin_amdgcn_readfirstlane(row0);
  const int first=tile[1]+start,live=min(BT,tile[2]-start);
  const float* in[BT];float acc[RW][BT]{};
  const uint16_t* qw[RW];const __half* sw[RW];
  #pragma unroll
  for(int t=0;t<BT;++t){
    const int slot=first+min(t,live-1);
    int ix=tokidx?tokidx[slot]:slot;
    if constexpr(UniformRows)ix=__builtin_amdgcn_readfirstlane(ix);
    in[t]=x+(size_t)ix*cols;
  }
  #pragma unroll
  for(int r=0;r<RW;++r){
    const int row=min(row0+r,erows-1);
    const size_t off=((size_t)tile[0]*erows+row)*cols;
    qw[r]=(const uint16_t*)(q+off/2);sw[r]=scales+off/128;
  }
  __shared__ float sx[StageX?BT*128*StageGroups:1];
  for(int g=0;g<cols/128;++g){
    if constexpr(StageX){if(g%StageGroups==0){
      for(int idx=threadIdx.x;idx<BT*128*StageGroups;idx+=NW*32){
        int c=g*128+idx%(128*StageGroups);
        sx[idx]=c<cols?in[idx/(128*StageGroups)][c]:0.f;
      }
      __syncthreads();
    }}
    uint32_t packed[RW];float scale[RW];
    #pragma unroll
    for(int r=0;r<RW;++r){if constexpr(!DirectQ)packed[r]=qw[r][g*32+lane];scale[r]=__half2float(sw[r][g]);}
    #pragma clang loop unroll_count(JUnroll)
    for(int j=0;j<4;++j){
      float w[RW];
      #pragma unroll
      for(int r=0;r<RW;++r){
        if constexpr(DirectQ){
          const uint32_t bits=((const uint32_t*)qw[r])[g*16+j*4+lane/8];
          w[r]=float(int((bits>>(4*(lane%8)))&15)-8)*scale[r];
        } else {
          const uint32_t bits=__shfl(packed[r],j*8+lane/4,32);
          w[r]=float(int((bits>>(4*(lane%4)))&15)-8)*scale[r];
        }
      }
      #pragma unroll
      for(int t=0;t<BT;++t){
        const float xv=StageX?sx[t*128*StageGroups+(g%StageGroups)*128+j*32+lane]:in[t][g*128+j*32+lane];
        #pragma unroll
        for(int r=0;r<RW;++r)acc[r][t]=fmaf(w[r],xv,acc[r][t]);
      }
    }
    if constexpr(StageX){if(g%StageGroups==StageGroups-1)__syncthreads();}
  }
  #pragma unroll
  for(int r=0;r<RW;++r){
    #pragma unroll
    for(int t=0;t<BT;++t){
      for(int h=16;h;h/=2)acc[r][t]+=__shfl_down(acc[r][t],h,32);
      if(lane==0 && t<live && row0+r<erows)
        y[(size_t)(first+t)*erows+row0+r]=acc[r][t];
    }
  }
}
} // namespace hgnv2gpu
