#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include "../../src/gpu/parts/28_kernels_hgn_v2.inc"
#include "../../src/gpu/parts/29_kernels_v2_exact_batch.inc"
#include "r21_cooperative_exact.inc"
#define CK(x) do{hipError_t _hip_result=(x);if(_hip_result!=hipSuccess)throw std::runtime_error(hipGetErrorString(_hip_result));}while(0)
__global__ void init_q(uint8_t* q,size_t n){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n)q[i]=uint8_t((i*1315423911ull+(i>>5)*71)^0xa7);}
__global__ void init_s(__half* s,size_t n){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n)s[i]=__float2half(float(int(i%61)-30)/256.f);}
__global__ void init_x(float* x,size_t n){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n)x[i]=float(int((i*1664525ull+1013904223ull)%65521)-32760)/17031.f;}
__global__ void check(const float* a,const float* b,size_t n,unsigned* diff){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n){if(!isfinite(b[i]))atomicAdd(diff+1,1u);if(__float_as_uint(a[i])!=__float_as_uint(b[i]))atomicAdd(diff,1u);}}

void run(int P,int E,int D,int M,bool gather){
  const int K=10,S=P*K;
  std::vector<int> tiles,ids(S);int cursor=0;
  for(int e=0;e<E;++e){int count=S/E+(e<S%E);for(int first=0;first<count;first+=64){tiles.insert(tiles.end(),{e,cursor+first,std::min(64,count-first)});}for(int j=0;j<count;++j)ids[cursor+j]=(cursor+j)*17%P;cursor+=count;}
  int nt=int(tiles.size()/3);uint8_t* q;__half* sc;float *x,*a,*b;int *ti,*di,*dn;unsigned* diff;
  const size_t weights=(size_t)E*M*D,inputs=(size_t)(gather?P:S)*D,outputs=(size_t)S*M;
  CK(hipMalloc(&q,weights/2));CK(hipMalloc(&sc,weights/128*2));CK(hipMalloc(&x,inputs*4));
  CK(hipMalloc(&a,(outputs+128)*4));CK(hipMalloc(&b,(outputs+128)*4));CK(hipMalloc(&ti,tiles.size()*4));CK(hipMalloc(&di,S*4));CK(hipMalloc(&dn,4));CK(hipMalloc(&diff,8));
  CK(hipMemcpy(ti,tiles.data(),tiles.size()*4,hipMemcpyHostToDevice));CK(hipMemcpy(di,ids.data(),S*4,hipMemcpyHostToDevice));CK(hipMemcpy(dn,&nt,4,hipMemcpyHostToDevice));
  init_q<<<(weights/2+255)/256,256>>>(q,weights/2);init_s<<<(weights/128+255)/256,256>>>(sc,weights/128);init_x<<<(inputs+255)/256,256>>>(x,inputs);
  CK(hipMemset(a,0xa5,(outputs+128)*4));CK(hipMemset(b,0xa5,(outputs+128)*4));
  auto baseline=[&]{
    if(gather)hgnv2gpu::expert_batch_exact_reuse<16,8,false,2,1,false,true,1><<<dim3((M+15)/16,nt,4),64>>>(q,sc,x,a+64,di,ti,dn,D,M);
    else hgnv2gpu::expert_batch_exact_reuse<8,4,false,4,1,true,true,4><<<dim3((M+15)/16,nt,8),128>>>(q,sc,x,a+64,nullptr,ti,dn,D,M);
  };
  baseline();CK(hipDeviceSynchronize());
  auto timing=[&](auto fn){hipEvent_t s,e;CK(hipEventCreate(&s));CK(hipEventCreate(&e));fn();CK(hipEventRecord(s));for(int k=0;k<3;++k)fn();CK(hipEventRecord(e));CK(hipEventSynchronize(e));float ms;CK(hipEventElapsedTime(&ms,s,e));CK(hipEventDestroy(s));CK(hipEventDestroy(e));return ms/3;};
  float base_ms=timing(baseline);
  auto candidate=[&](auto bt,auto rw,auto stage,auto nw,auto sg,auto dq,auto ur,auto ju,auto go){
    constexpr int B=decltype(bt)::value,V=decltype(rw)::value,W=decltype(nw)::value,G=decltype(sg)::value,J=decltype(ju)::value,O=decltype(go)::value;constexpr bool L=decltype(stage)::value,Q=decltype(dq)::value,U=decltype(ur)::value;
    auto fn=[&]{hgnv2gpu::expert_cooperative<B,V,W,G,L><<<dim3((M+V*W-1)/(V*W),nt,(64+B*G-1)/(B*G)),32*W*G>>>(q,sc,x,b+64,gather?di:nullptr,ti,dn,D,M);};
    CK(hipMemset(b,0xa5,(outputs+128)*4));fn();CK(hipMemset(diff,0,8));check<<<(outputs+128+255)/256,256>>>(a,b,outputs+128,diff);CK(hipDeviceSynchronize());unsigned result[2];CK(hipMemcpy(result,diff,8,hipMemcpyDeviceToHost));
    // 0xa5 guard words are finite; compare includes both guards and all tails.
    if(result[0]||result[1])throw std::runtime_error("exact candidate differs");
    float ms=timing(fn);float fresh_base=timing(baseline);
    hipFuncAttributes attr{};CK(hipFuncGetAttributes(&attr,(const void*)hgnv2gpu::expert_cooperative<B,V,W,G,L>));
    printf("P=%d E=%d D=%d M=%d gather=%d BT=%d RW=%d LDS=%d NW=%d SG=%d direct=%d uniform=%d unroll=%d order=%d regs=%d local=%zu unequal=%u nonfinite=%u baseline_ms=%.6f candidate_ms=%.6f speedup=%.5f\n",P,E,D,M,gather,B,V,L,W,G,Q,U,J,O,attr.numRegs,attr.localSizeBytes,result[0],result[1],(base_ms+fresh_base)/2,ms,(base_ms+fresh_base)/2/ms);fflush(stdout);base_ms=fresh_base;
  };
  #define RUN(B,V,L,W,G,Q,U,J,O) candidate(std::integral_constant<int,B>{},std::integral_constant<int,V>{},std::integral_constant<bool,L>{},std::integral_constant<int,W>{},std::integral_constant<int,G>{},std::integral_constant<bool,Q>{},std::integral_constant<bool,U>{},std::integral_constant<int,J>{},std::integral_constant<int,O>{})
  RUN(8,4,false,4,2,false,false,1,0);
  RUN(8,4,true,4,2,false,false,1,0);
  RUN(8,4,false,2,4,false,false,1,0);
  RUN(8,4,true,2,4,false,false,1,0);
  RUN(16,4,false,4,2,false,false,1,0);
  RUN(16,4,true,4,2,false,false,1,0);
  RUN(8,8,false,2,2,false,false,1,0);
  RUN(8,8,true,2,2,false,false,1,0);
  RUN(16,8,false,2,2,false,false,1,0);
  RUN(16,8,true,2,2,false,false,1,0);
  RUN(8,4,false,4,1,false,false,1,0);
  RUN(8,4,true,4,1,false,false,1,0);
  #undef RUN
  for(void* ptr:{(void*)q,(void*)sc,(void*)x,(void*)a,(void*)b,(void*)ti,(void*)di,(void*)dn,(void*)diff})CK(hipFree(ptr));
}
int main(){try{run(67,17,128,130,true);run(513,512,2560,1280,true);run(8192,512,2560,1280,true);run(513,512,640,2560,false);run(8192,512,640,2560,false);puts("ALL PASS");return 0;}catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
