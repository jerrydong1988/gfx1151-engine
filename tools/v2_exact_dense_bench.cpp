#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include "../src/gpu/parts/28_kernels_hgn_v2.inc"
#include "../src/gpu/parts/29_kernels_v2_exact_batch.inc"
#define CK(x) do{hipError_t _hip_result=(x);if(_hip_result!=hipSuccess)throw std::runtime_error(hipGetErrorString(_hip_result));}while(0)
__global__ void init_codes(uint32_t* p,size_t n,unsigned seed){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n)p[i]=(i*1664525ull+1013904223ull)^(i>>9)^seed;}
__global__ void init_scale(__half* h,float* f,int n,unsigned seed){int i=blockIdx.x*256+threadIdx.x;if(i<n){h[i]=__float2half(float(int((i*17+seed)%251)-125)/71.f);f[i]=__half2float(h[i]);}}
__global__ void cmp(const uint16_t* a,const uint16_t* b,size_t n,unsigned* d){size_t i=(size_t)blockIdx.x*256+threadIdx.x;if(i<n && a[i]!=b[i])atomicAdd(d,1u);}
void run(int rows,int cols,unsigned seed){
  size_t n=(size_t)rows*cols;uint32_t* q;__half *su,*sv;float *fu,*fv;uint16_t *a,*b;unsigned* diff;
  CK(hipMalloc(&q,n/2));CK(hipMalloc(&su,cols*2));CK(hipMalloc(&sv,rows*2));CK(hipMalloc(&fu,cols*4));CK(hipMalloc(&fv,rows*4));CK(hipMalloc(&a,(n+128)*2));CK(hipMalloc(&b,(n+128)*2));CK(hipMalloc(&diff,4));
  init_codes<<<(n/8+255)/256,256>>>(q,n/8,seed);init_scale<<<(cols+255)/256,256>>>(su,fu,cols,seed);init_scale<<<(rows+255)/256,256>>>(sv,fv,rows,seed+57);
  CK(hipMemset(a,0x35,(n+128)*2));CK(hipMemset(b,0x35,(n+128)*2));
  auto old=[&]{hgnv2gpu::ht_dense<<<dim3(cols/128,rows/128),256>>>(q,su,sv,a+64,cols);};
  auto opt=[&]{hgnv2gpu::ht_dense_exact_streamed<<<rows/128*(cols/128),256>>>((const uint8_t*)q,fu,fv,b+64,cols);};
  old();opt();CK(hipMemset(diff,0,4));cmp<<<(n+128+255)/256,256>>>(a,b,n+128,diff);CK(hipDeviceSynchronize());unsigned count;CK(hipMemcpy(&count,diff,4,hipMemcpyDeviceToHost));
  if(count){printf("rows=%d cols=%d seed=%u unequal=%u\n",rows,cols,seed,count);throw std::runtime_error("weight decode mismatch");}
  auto time=[&](auto fn){hipEvent_t s,e;CK(hipEventCreate(&s));CK(hipEventCreate(&e));fn();CK(hipEventRecord(s));for(int i=0;i<10;++i)fn();CK(hipEventRecord(e));CK(hipEventSynchronize(e));float ms;CK(hipEventElapsedTime(&ms,s,e));CK(hipEventDestroy(s));CK(hipEventDestroy(e));return ms/10;};
  float old_ms=time(old),new_ms=time(opt);printf("rows=%d cols=%d seed=%u unequal=0 old_ms=%.6f candidate_ms=%.6f speedup=%.3f\n",rows,cols,seed,old_ms,new_ms,old_ms/new_ms);fflush(stdout);
  for(void* p:{(void*)q,(void*)su,(void*)sv,(void*)fu,(void*)fv,(void*)a,(void*)b,(void*)diff})CK(hipFree(p));
}
int main(){try{for(unsigned seed:{0u,131u,0xdeadbeefu}){run(128,128,seed);run(640,2560,seed);run(2560,6144,seed);run(12288,2560,seed);}puts("ALL PASS");return 0;}catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
