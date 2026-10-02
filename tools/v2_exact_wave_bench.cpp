#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include "../src/gpu/parts/28_kernels_hgn_v2.inc"
#include "../src/gpu/parts/29_kernels_v2_exact_batch.inc"
#include "../src/gpu/parts/29_kernels_v2_exact_wave.inc"
#include "../build/reduce_reference.inc"
#define CK(e) do{auto _hr=(e);if(_hr!=hipSuccess){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,hipGetErrorString(_hr));exit(2);}}while(0)
__global__ void init(float* x,size_t n,int seed) {
  for(size_t i=(size_t)blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=(size_t)gridDim.x*blockDim.x) {
    unsigned v=(unsigned)i*1664525u+1013904223u+seed;v^=v>>15;
    x[i]=float(int(v%2039)-1019)/3091.f;
    if(i%127==0)x[i]=__uint_as_float(i&1?0x80000000u:0u);
    if(i%1021==0)x[i]=__uint_as_float(0x80000001u+(i%256));
  }
}
template<class F> float timeit(F fn,int count) {
  hipEvent_t a,b;CK(hipEventCreate(&a));CK(hipEventCreate(&b));std::vector<float> ms;
  fn();CK(hipDeviceSynchronize());
  for(int r=0;r<3;++r){CK(hipEventRecord(a));for(int t=0;t<count;++t)fn();CK(hipEventRecord(b));CK(hipEventSynchronize(b));float v;CK(hipEventElapsedTime(&v,a,b));ms.push_back(v/count);}
  CK(hipEventDestroy(a));CK(hipEventDestroy(b));std::sort(ms.begin(),ms.end());return ms[1];
}
void run(int rows,int cols,int kind,int seed) {
  size_t n=(size_t)rows*cols;float *x,*a,*b;__half *s,*s2;
  CK(hipMalloc(&x,n*8));CK(hipMalloc(&a,(n+64)*4));CK(hipMalloc(&b,(n+64)*4));CK(hipMalloc(&s,cols*4));CK(hipMalloc(&s2,cols*2));
  std::vector<__half> hs(cols*2);for(int i=0;i<cols*2;++i)hs[i]=__float2half((i%3==0?-1.f:1.f)*(1+(i%7)*0.125f));
  CK(hipMemcpy(s,hs.data(),cols*4,hipMemcpyHostToDevice));CK(hipMemcpy(s2,hs.data()+cols,cols*2,hipMemcpyHostToDevice));
  init<<<2048,256>>>(x,n*2,seed);
  auto baseline=[&]{if(kind==2)hgnv2gpu::activate_rotate<<<dim3(cols/128,rows),128>>>(x,a,s,s2,cols);else hgnv2gpu::rotate_fast<<<dim3(cols/128,rows),128>>>(x,a,s,cols,kind==1);};
  int loops=n>10000000?4:50;
  float base_ms=timeit(baseline,loops);
  std::vector<uint32_t> ref(n),v(n+64);CK(hipMemcpy(ref.data(),a,n*4,hipMemcpyDeviceToHost));
  for(int nw:{2,4,8}) {
    auto candidate=[&]{
      #define LAUNCH(NW) if(kind==2)hgnv2gpu::exact_activate_wave<NW><<<(rows*(cols/128)+NW-1)/NW,NW*32>>>(x,b,s,s2,cols,rows);else hgnv2gpu::exact_rotate_wave<NW><<<(rows*(cols/128)+NW-1)/NW,NW*32>>>(x,b,s,cols,rows,kind==1)
      if(nw==2){LAUNCH(2);}else if(nw==4){LAUNCH(4);}else{LAUNCH(8);}
      #undef LAUNCH
    };
    CK(hipMemset(b,0xa5,(n+64)*4));candidate();CK(hipMemcpy(v.data(),b,(n+64)*4,hipMemcpyDeviceToHost));
    size_t bad=0,nan=0,guard=0;for(size_t i=0;i<n;++i){bad+=ref[i]!=v[i];nan+=(v[i]&0x7f800000)==0x7f800000;}for(size_t i=n;i<n+64;++i)guard+=v[i]!=0xa5a5a5a5;
    float ms=timeit(candidate,loops);
    printf("{\"rows\":%d,\"cols\":%d,\"kind\":%d,\"seed\":%d,\"nw\":%d,\"base_ms\":%.6f,\"ms\":%.6f,\"bad\":%zu,\"nonfinite\":%zu,\"guard\":%zu}\n",rows,cols,kind,seed,nw,base_ms,ms,bad,nan,guard);fflush(stdout);
    if(bad||nan||guard)exit(3);
  }
  CK(hipFree(x));CK(hipFree(a));CK(hipFree(b));CK(hipFree(s));CK(hipFree(s2));
}
void reduce(int rows,int cols,int seed) {
  constexpr int K=10;size_t n=(size_t)rows*cols,np=n*K;
  float *x,*tmp,*a,*b,*pw;int* ids;__half* signs;
  CK(hipMalloc(&x,np*4));CK(hipMalloc(&tmp,np*4));CK(hipMalloc(&a,n*4));CK(hipMalloc(&b,(n+64)*4));CK(hipMalloc(&pw,rows*K*4));CK(hipMalloc(&ids,rows*K*4));CK(hipMalloc(&signs,cols*2));
  std::vector<int> hi(rows*K);std::vector<float> hw(rows*K);std::vector<__half> hs(cols);
  for(int i=0;i<rows*K;++i){hi[i]=rows*K-1-i;hw[i]=float(i%31-15)/29.f;}
  std::shuffle(hi.begin(),hi.end(),std::mt19937(seed));
  for(int i=0;i<cols;++i)hs[i]=__float2half((i%3==0?-1.f:1.f)*(1+(i%7)*0.125f));
  CK(hipMemcpy(ids,hi.data(),hi.size()*4,hipMemcpyHostToDevice));CK(hipMemcpy(pw,hw.data(),hw.size()*4,hipMemcpyHostToDevice));CK(hipMemcpy(signs,hs.data(),hs.size()*2,hipMemcpyHostToDevice));init<<<2048,256>>>(x,np,seed);
  auto baseline=[&]{hgnv2gpu::rotate_fast<<<dim3(cols/128,rows*K),128>>>(x,tmp,signs,cols,true);k_moe_reduce_pw<K><<<dim3((cols+255)/256,rows),256>>>(tmp,ids,pw,a,rows,cols);};
  int loops=np>10000000?4:50;float base=timeit(baseline,loops);std::vector<uint32_t> ref(n),v(n+64);CK(hipMemcpy(ref.data(),a,n*4,hipMemcpyDeviceToHost));
  for(int nw:{2,4,8}) {
    auto candidate=[&]{
      #define INV(NW) hgnv2gpu::exact_inverse_reduce_wave<K,NW><<<(rows*(cols/128)+NW-1)/NW,NW*32>>>(x,ids,pw,signs,b,rows,cols)
      if(nw==2){INV(2);}else if(nw==4){INV(4);}else{INV(8);}
      #undef INV
    };
    CK(hipMemset(b,0xa5,(n+64)*4));candidate();CK(hipMemcpy(v.data(),b,(n+64)*4,hipMemcpyDeviceToHost));size_t bad=0,nan=0,guard=0;
    for(size_t i=0;i<n;++i){bad+=v[i]!=ref[i];nan+=(v[i]&0x7f800000)==0x7f800000;}for(size_t i=n;i<n+64;++i)guard+=v[i]!=0xa5a5a5a5;
    float ms=timeit(candidate,loops);printf("{\"rows\":%d,\"cols\":%d,\"kind\":3,\"seed\":%d,\"nw\":%d,\"base_ms\":%.6f,\"ms\":%.6f,\"bad\":%zu,\"nonfinite\":%zu,\"guard\":%zu}\n",rows,cols,seed,nw,base,ms,bad,nan,guard);fflush(stdout);if(bad||nan||guard)exit(3);
  }
  CK(hipFree(x));CK(hipFree(tmp));CK(hipFree(a));CK(hipFree(b));CK(hipFree(pw));CK(hipFree(ids));CK(hipFree(signs));
}
int main(int argc,char**) {
  if(argc>1){for(int seed:{13,721})for(int rows:{1,16,65,513,8192})reduce(rows,2560,seed);return 0;}
  for(int seed:{13,721})for(int rows:{1,10,513,8192,81920}) {run(rows,2560,0,seed);run(rows,2560,1,seed);run(rows,640,2,seed);}
}
