// Diagnostic microbenchmark: preserve the serial BF16 GEMV FMA/reduction order.
// Rotate >96 MiB weights for normal shapes. Small tails primarily test correctness.
#include <hip/hip_runtime.h>
#include <hip/hip_bf16.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
#include "../build/bf16_reference.inc"
#define CK(e) do { auto _hr=(e); if(_hr!=hipSuccess) {fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,hipGetErrorString(_hr));exit(2);} } while(0)

#include "../src/gpu/parts/20_kernels_bf16_exact.inc"

__global__ void init_w(uint16_t* w,size_t n,unsigned seed) {
  for(size_t i=(size_t)blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=(size_t)gridDim.x*blockDim.x) {
    unsigned v=(unsigned)i*1664525u+1013904223u+seed; v^=v>>15;v*=2246822519u;v^=v>>13;
    // Finite signs and broad exponents; inputs include zero/cancellation cases.
    w[i]=(v&0x8000u)|(((v>>8)%24+113)<<7)|(v&127);
  }
}
struct Variant {const char* name; std::function<void(const uint16_t*,const float*,float*,int,int,int)> launch;};
template<int P> std::vector<Variant> variants() {
  std::vector<Variant> v;
  v.push_back({"original",[](auto w,auto x,auto y,int n,int k,int s){
    if constexpr(P==1) k_bf16_gemv<<<(n+7)/8,512>>>(w,x,y,n,k);
    else k_bf16_gemv_mr<P,true><<<(n+31)/32,512>>>(w,x,y,n,k,s);
  }});
  for(int bs:{64,128,256,512}) {
    const char* nm=bs==64?"mr64":bs==128?"mr128":bs==256?"mr256":"mr512";
    v.push_back({nm,[bs](auto w,auto x,auto y,int n,int k,int s){k_bf16_gemv_mr<P,true><<<(n+bs/16-1)/(bs/16),bs>>>(w,x,y,n,k,s);}});
  }
  #define V(RW,NW,PF) v.push_back({"rw" #RW "nw" #NW "pf" #PF,[](auto w,auto x,auto y,int n,int k,int s){exact_gemv<P,RW,NW,PF><<<(n+RW*NW-1)/(RW*NW),NW*32>>>(w,x,y,n,k,s);}})
  V(1,2,false); V(1,4,false); V(1,8,false);
  V(2,2,false); V(2,4,false); V(2,8,false);
  V(4,2,false); V(4,4,false); V(4,8,false);
  V(2,4,true); V(4,4,true); V(8,4,false);
  #undef V
  return v;
}
template<int P> void bench(int n,int k) {
  const int xs=k+16;
  const size_t wn=(size_t)n*k;
  const int copies=std::min(128,std::max(1,int((96ull<<20)/(wn*2))+1));
  uint16_t* w; float *x,*y,*ref;
  CK(hipMalloc(&w,wn*copies*2));CK(hipMalloc(&x,(size_t)P*xs*4));CK(hipMalloc(&y,((size_t)P*n+64)*4));CK(hipMalloc(&ref,(size_t)P*n*4));
  std::vector<float> hx(P*xs);
  for(size_t i=0;i<hx.size();++i)hx[i]=float(int((i*97)%101)-50)*0.03125f;
  CK(hipMemcpy(x,hx.data(),hx.size()*4,hipMemcpyHostToDevice));
  init_w<<<2048,256>>>(w,wn*copies,239);
  auto vs=variants<P>();
  std::vector<uint32_t> expected((size_t)P*n),actual((size_t)P*n+64);
  // Independent per-token original serial kernels define the reference.
  for(int p=0;p<P;++p)k_bf16_gemv<<<(n+7)/8,512>>>(w,x+(size_t)p*xs,ref+(size_t)p*n,n,k);
  CK(hipMemcpy(expected.data(),ref,expected.size()*4,hipMemcpyDeviceToHost));
  int loops=std::min(128,std::max(8,int((256ull<<20)/(wn*2))+1));
  hipEvent_t st,en; CK(hipEventCreate(&st));CK(hipEventCreate(&en));
  for(auto& v:vs) {
    CK(hipMemset(y,0xA5,actual.size()*4));
    v.launch(w,x,y,n,k,xs);CK(hipMemcpy(actual.data(),y,actual.size()*4,hipMemcpyDeviceToHost));
    size_t bad=0,nonfinite=0,guard=0;
    for(size_t i=0;i<expected.size();++i) {bad+=expected[i]!=actual[i];nonfinite+=(actual[i]&0x7f800000)==0x7f800000;}
    for(size_t i=expected.size();i<actual.size();++i)guard+=actual[i]!=0xa5a5a5a5;
    std::vector<float> ms;
    for(int rep=0;rep<3;++rep) {
      CK(hipEventRecord(st));
      for(int t=0;t<loops;++t)v.launch(w+(size_t)((t+rep*loops)%copies)*wn,x,y,n,k,xs);
      CK(hipEventRecord(en));CK(hipEventSynchronize(en));float m;CK(hipEventElapsedTime(&m,st,en));ms.push_back(m/loops);
    }
    std::sort(ms.begin(),ms.end());
    printf("{\"p\":%d,\"n\":%d,\"k\":%d,\"copies\":%d,\"variant\":\"%s\",\"ms\":%.6f,\"min_ms\":%.6f,\"max_ms\":%.6f,\"bad\":%zu,\"nonfinite\":%zu,\"guard\":%zu}\n",P,n,k,copies,v.name,ms[1],ms[0],ms[2],bad,nonfinite,guard);fflush(stdout);
    if(bad||nonfinite||guard)exit(3);
  }
  CK(hipEventDestroy(st));CK(hipEventDestroy(en));CK(hipFree(w));CK(hipFree(x));CK(hipFree(y));CK(hipFree(ref));
}
int main() {
  for(auto s:std::vector<std::pair<int,int>>{{2560,6144},{2560,640},{640,2560},{320,10240},{10240,320},{512,2560},{10240,2560},{6144,2560},{12288,2560},{1,320},{35,640},{248320,2560}}) {
    bench<1>(s.first,s.second);bench<2>(s.first,s.second);bench<4>(s.first,s.second);bench<8>(s.first,s.second);
  }
}
