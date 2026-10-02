// Opt-in exact BF16 GEMV layout. Each lane retains the original c/j FMA
// sequence and the 16,8,4,2,1 shuffle reduction.
template<int P,int RW,int NW,bool Prefetch=false>
__global__ __launch_bounds__(NW*32) void exact_gemv(const uint16_t* __restrict__ w,
    const float* __restrict__ x,float* __restrict__ y,int n,int k,int xs) {
  const int lane=threadIdx.x&31;
  const int row=(blockIdx.x*NW+threadIdx.x/32)*RW;
  if(row>=n)return;
  float acc[P][RW]={};
  uint4 next[RW];
  if constexpr(Prefetch) {
    if(lane*8<k) {
      #pragma unroll
      for(int r=0;r<RW;++r)next[r]=row+r<n?*(const uint4*)(w+(size_t)(row+r)*k+lane*8):make_uint4(0,0,0,0);
    }
  }
  for(int c=lane*8;c<k;c+=256) {
    uint4 pk[RW];
    #pragma unroll
    for(int r=0;r<RW;++r) {
      if constexpr(Prefetch) {
        pk[r]=next[r];
        if(c+256<k)next[r]=row+r<n?*(const uint4*)(w+(size_t)(row+r)*k+c+256):make_uint4(0,0,0,0);
      } else pk[r]=row+r<n?*(const uint4*)(w+(size_t)(row+r)*k+c):make_uint4(0,0,0,0);
    }
    #pragma unroll
    for(int p=0;p<P;++p) {
      float4 a=*(const float4*)(x+(size_t)p*xs+c),b=*(const float4*)(x+(size_t)p*xs+c+4);
      const float xv[8]={a.x,a.y,a.z,a.w,b.x,b.y,b.z,b.w};
      #pragma unroll
      for(int j=0;j<8;++j) {
        #pragma unroll
        for(int r=0;r<RW;++r) {
          uint32_t u=(uint32_t)((const uint16_t*)&pk[r])[j]<<16;
          float wf; memcpy(&wf,&u,4);
          acc[p][r]+=wf*xv[j];
        }
      }
    }
  }
  #pragma unroll
  for(int p=0;p<P;++p) {
    #pragma unroll
    for(int r=0;r<RW;++r) {
      for(int off=16;off;off>>=1)acc[p][r]+=__shfl_down_sync(~0ull,acc[p][r],off);
      if(lane==0 && row+r<n)y[(size_t)p*n+row+r]=acc[p][r];
    }
  }
}
