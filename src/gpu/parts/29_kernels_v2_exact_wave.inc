// Exact wave-local 128-value transforms. Four FP32 registers per lane retain
// all seven reference butterfly stages, scale placement, and activation order.
// No FP16/BF16 intermediate or changed reduction association is introduced.
namespace hgnv2gpu {
template<int NW=4>
__global__ __launch_bounds__(NW*32) void exact_rotate_wave(
    const float* x,float* y,const __half* sign,int cols,int rows,bool inverse) {
  const int tile=blockIdx.x*NW+threadIdx.x/32,lane=threadIdx.x&31;
  if(tile>=rows*(cols/128))return;
  const int cb=(tile%(cols/128))*128;
  const size_t base=(size_t)tile*128;
  float v[4];
  #pragma unroll
  for(int j=0;j<4;++j)v[j]=x[base+j*32+lane]*(inverse?1.f:__half2float(sign[cb+j*32+lane]));
  exact_fwht128(v,lane);
  #pragma unroll
  for(int j=0;j<4;++j)y[base+j*32+lane]=(v[j]*0.08838834764831844f)*(inverse?__half2float(sign[cb+j*32+lane]):1.f);
}
template<int NW=4>
__global__ __launch_bounds__(NW*32) void exact_activate_wave(
    const float* gu,float* hid,const __half* gu_sv,const __half* dn_su,int mid,int rows) {
  const int tile=blockIdx.x*NW+threadIdx.x/32,lane=threadIdx.x&31;
  if(tile>=rows*(mid/128))return;
  const int row=tile/(mid/128),cb=(tile%(mid/128))*128;
  const size_t base=(size_t)row*2*mid+cb;
  float g[4],u[4],v[4];
  #pragma unroll
  for(int j=0;j<4;++j){g[j]=gu[base+j*32+lane];u[j]=gu[base+mid+j*32+lane];}
  exact_fwht128(g,lane);exact_fwht128(u,lane);
  #pragma unroll
  for(int j=0;j<4;++j) {
    const int c=cb+j*32+lane;
    const float gg=(g[j]*0.08838834764831844f)*__half2float(gu_sv[c]);
    const float uu=(u[j]*0.08838834764831844f)*__half2float(gu_sv[c+mid]);
    v[j]=(gg/(1.f+expf(-gg)))*uu*__half2float(dn_su[c]);
  }
  exact_fwht128(v,lane);
  #pragma unroll
  for(int j=0;j<4;++j)hid[(size_t)row*mid+cb+j*32+lane]=v[j]*0.08838834764831844f;
}
// Invert EACH expert pair before rank-ordered FMA, as in rotate_fast followed
// by k_moe_reduce_pw. Moving the transform after the sum would not be exact.
template<int K,int NW=4>
__global__ __launch_bounds__(NW*32) void exact_inverse_reduce_wave(
    const float* __restrict__ pairs,const int* __restrict__ pairids,
    const float* __restrict__ pw,const __half* __restrict__ sign,
    float* __restrict__ out,int rows,int cols) {
  const int tile=blockIdx.x*NW+threadIdx.x/32,lane=threadIdx.x&31;
  if(tile>=rows*(cols/128))return;
  const int row=tile/(cols/128),cb=(tile%(cols/128))*128;
  float sum[4]={};
  #pragma unroll
  for(int s=0;s<K;++s) {
    const int ix=__builtin_amdgcn_readfirstlane(pairids[row*K+s]);
    const float weight=pw[ix];float v[4];
    #pragma unroll
    for(int j=0;j<4;++j)v[j]=pairs[(size_t)ix*cols+cb+j*32+lane]*1.f;
    exact_fwht128(v,lane);
    #pragma unroll
    for(int j=0;j<4;++j) {
      const float inv=(v[j]*0.08838834764831844f)*__half2float(sign[cb+j*32+lane]);
      sum[j]+=weight*inv;
    }
  }
  #pragma unroll
  for(int j=0;j<4;++j)out[(size_t)row*cols+cb+j*32+lane]=sum[j];
}
} // namespace hgnv2gpu
