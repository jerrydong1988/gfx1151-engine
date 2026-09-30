// Differential checks and device-event timing for the complete rotated MoE.
// Only synthetic, deterministic data; no model/service needed.
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>
#include <stdexcept>
#include <type_traits>
struct MoeTile { int expert, first, count; };
#include "../src/gpu/parts/26_kernels_moe_gguf.inc"
#include "../src/gpu/parts/27_kernels_moe_lut.inc"
#include "../src/gpu/parts/28_kernels_hgn_v2.inc"
#define CK(x) do {auto err_=(x);if(err_!=hipSuccess)throw std::runtime_error(hipGetErrorString(err_));}while(0)
struct Arena {
  std::vector<void*> p;
  template<class T>T* put(const std::vector<T>& v){T* q;CK(hipMalloc(&q,v.size()*sizeof(T)));p.push_back(q);CK(hipMemcpy(q,v.data(),v.size()*sizeof(T),hipMemcpyHostToDevice));return q;}
  template<class T>T* alloc(size_t n){T* q;CK(hipMalloc(&q,n*sizeof(T)));p.push_back(q);return q;}
  ~Arena(){for(auto q:p)(void)hipFree(q);}
};
__global__ void act_ref(const float* gu,float* h,int m){
  int c=blockIdx.x*256+threadIdx.x,s=blockIdx.y;if(c<m){float g=gu[(size_t)s*2*m+c];h[(size_t)s*m+c]=g/(1.f+expf(-g))*gu[(size_t)s*2*m+m+c];}
}
__global__ void reduce_sorted(const float* pairs,const int* order,const float* w,float* y,int d,int k){
  int c=blockIdx.x*256+threadIdx.x,t=blockIdx.y;if(c>=d)return;float v=0;
  for(int j=0;j<k;j++)v=fmaf(pairs[(size_t)order[t*k+j]*d+c],w[t*16+j],v);
  y[(size_t)t*d+c]=v;
}
static void test(int P,int E,int D,int M,int reps){
  const int K=10,S=P*K;std::mt19937 rng(901+P);
  auto weights=[&](int rows,int cols){
    size_t nc=(size_t)E*rows*cols/2;std::vector<uint8_t> a(nc+(size_t)E*rows*cols/64);
    for(size_t i=0;i<nc;i++)a[i]=uint8_t(rng());
    for(size_t i=nc;i<a.size();i+=2){uint16_t h=__builtin_bit_cast(uint16_t,__float2half((int(rng()%200)-100)*.00004f));memcpy(a.data()+i,&h,2);}return a;
  };
  auto g=weights(2*M,D),down=weights(D,M);
  auto signs=[&](int n,bool scaled){std::vector<__half> v(n);for(auto& a:v)a=__float2half((rng()%2?-1.f:1.f)*(scaled?.5f:1.f));return v;};
  auto a=signs(D,false),b=signs(2*M,true),c=signs(M,false),d=signs(D,true);
  std::vector<float> x((size_t)P*D),w(P*16,0);for(auto& v:x)v=float(int(rng()%200)-100)*.02f;
  std::vector<int> ids(P*16),idx,order(S),expert(E);std::vector<std::vector<int>> slots(E);
  for(int e=0;e<E;e++)expert[e]=e;
  for(int t=0;t<P;t++){std::shuffle(expert.begin(),expert.end(),rng);for(int k=0;k<K;k++){ids[t*16+k]=expert[k];w[t*16+k]=(k+1)*.018f;slots[expert[k]].push_back(t*K+k);}}
  std::vector<MoeTile> tiles;
  for(int e=0;e<E;e++){int start=(int)idx.size();for(int s:slots[e]){order[s]=(int)idx.size();idx.push_back(s/K);}for(int j=0;j<(int)slots[e].size();j+=64)tiles.push_back({e,start+j,std::min(64,(int)slots[e].size()-j)});}
  Arena mem;auto q=mem.put(g),qd=mem.put(down);auto su=mem.put(a),sv=mem.put(b),du=mem.put(c),dv=mem.put(d);
  auto dx=mem.put(x),dw=mem.put(w);auto di=mem.put(ids),ix=mem.put(idx),ord=mem.put(order);auto dt=mem.put(tiles);auto nt=mem.put(std::vector<int>{(int)tiles.size()});
  auto xr=mem.alloc<float>((size_t)P*D),gu=mem.alloc<float>((size_t)S*2*M),hid=mem.alloc<float>((size_t)S*M),pairs=mem.alloc<float>((size_t)S*D),out=mem.alloc<float>((size_t)P*D);
  auto x16=mem.alloc<__half>((size_t)P*D),h16=mem.alloc<__half>((size_t)S*M);
  const __half* gs=(__half*)(q+(size_t)E*2*M*D/2);const __half* ds=(__half*)(qd+(size_t)E*D*M/2);
  moelut::LutW vg{},vd{};vg.w=q;vg.sc=(const uint8_t*)gs;vg.sstride=D/64;vg.e_rows=2*M;vd.w=qd;vd.sc=(const uint8_t*)ds;vd.sstride=M/64;vd.e_rows=D;
  auto ref=[&]{
    hgnv2gpu::rotate<<<dim3(D/128,P),128>>>(dx,xr,su,D,P,false);
    hgnv2gpu::expert_mv<<<dim3((2*M+7)/8,S),256>>>(q,gs,xr,gu,di,D,2*M,K,S,false);
    hgnv2gpu::rotate<<<dim3(2*M/128,S),128>>>(gu,gu,sv,2*M,S,true);
    act_ref<<<dim3((M+255)/256,S),256>>>(gu,hid,M);
    hgnv2gpu::rotate<<<dim3(M/128,S),128>>>(hid,hid,du,M,S,false);
    hgnv2gpu::expert_mv<<<dim3((D+7)/8,S),256>>>(qd,ds,hid,pairs,di,M,D,K,S,true);
    hgnv2gpu::rotate<<<dim3(D/128,S),128>>>(pairs,pairs,dv,D,S,true);
    hgnv2gpu::reduce<<<dim3((D+255)/256,P),256>>>(pairs,dw,out,D,K);
  };
  auto variant=[&](auto precision,auto width){
    constexpr bool Precise=decltype(precision)::value;constexpr int BN=decltype(width)::value;
    using Act=std::conditional_t<Precise,float,__half>;
    Act* ax=Precise?(Act*)xr:(Act*)x16;Act* ah=Precise?(Act*)hid:(Act*)h16;
    hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(dx,ax,su,D,false);
    k_moe_lut<moelut::kQ4R128,false,BN,Precise><<<dim3(2*M/128,tiles.size(),64/BN),256>>>(vg,ax,ix,dt,nt,gu,nullptr,2*M,D);
    hgnv2gpu::activate_rotate<<<dim3(M/128,S),128>>>(gu,ah,sv,du,M);
    k_moe_lut<moelut::kQ4R128,false,BN,Precise><<<dim3(D/128,tiles.size(),64/BN),256>>>(vd,ah,nullptr,dt,nt,pairs,nullptr,D,M);
    reduce_sorted<<<dim3((D+255)/256,P),256>>>(pairs,ord,dw,out,D,K);
    hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(out,out,dv,D,true);
  };
  auto fast=[&]{variant(std::true_type{},std::integral_constant<int,64>{});};
  auto mv=[&]{
    hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(dx,xr,su,D,false);
    hgnv2gpu::expert_mv_fast<<<dim3((2*M+7)/8,S),128>>>(q,gs,xr,gu,di,D,2*M,K,S,false);
    hgnv2gpu::activate_rotate<<<dim3(M/128,S),128>>>(gu,hid,sv,du,M);
    hgnv2gpu::expert_mv_fast<<<dim3((D+7)/8,S),128>>>(qd,ds,hid,pairs,di,M,D,K,S,true);
    hgnv2gpu::reduce<<<dim3((D+255)/256,P),256>>>(pairs,dw,out,D,K);
    hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(out,out,dv,D,true);
  };
  auto exact=[&]{
    hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(dx,xr,su,D,false);
    hgnv2gpu::expert_batch_exact<8,true><<<dim3((2*M+7)/8,tiles.size(),8),256>>>(q,gs,xr,gu,ix,(int*)dt,nt,D,2*M);
    hgnv2gpu::activate_rotate<<<dim3(M/128,S),128>>>(gu,hid,sv,du,M);
    hgnv2gpu::expert_batch_exact<8,true><<<dim3((D+7)/8,tiles.size(),8),256>>>(qd,ds,hid,pairs,nullptr,(int*)dt,nt,M,D);
    hgnv2gpu::rotate_fast<<<dim3(D/128,S),128>>>(pairs,pairs,dv,D,true);
    reduce_sorted<<<dim3((D+255)/256,P),256>>>(pairs,ord,dw,out,D,K);
  };
  ref();CK(hipGetLastError());CK(hipDeviceSynchronize());std::vector<float> base((size_t)P*D),got(base.size());CK(hipMemcpy(base.data(),out,base.size()*4,hipMemcpyDeviceToHost));
  CK(hipMemset(out,0xff,got.size()*4));fast();CK(hipGetLastError());CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*4,hipMemcpyDeviceToHost));
  double err=0,norm=0;for(size_t i=0;i<base.size();i++){if(!std::isfinite(got[i]))throw std::runtime_error("nonfinite output");err+=(got[i]-base[i])*(got[i]-base[i]);norm+=base[i]*base[i];}
  double rel=sqrt(err/norm);if(rel>3e-5)throw std::runtime_error("WMMA MoE error "+std::to_string(rel));
  mv();CK(hipGetLastError());CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*4,hipMemcpyDeviceToHost));
  double me=0;for(size_t i=0;i<base.size();i++){if(!std::isfinite(got[i]))throw std::runtime_error("nonfinite MV");me+=(got[i]-base[i])*(got[i]-base[i]);}
  double mrel=sqrt(me/norm);if(mrel>1e-5)throw std::runtime_error("MV error "+std::to_string(mrel));
  exact();CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*4,hipMemcpyDeviceToHost));
  int unequal=0;double ee=0;for(size_t i=0;i<base.size();i++){unequal+=got[i]!=base[i];ee+=(got[i]-base[i])*(got[i]-base[i]);}
  double erel=sqrt(ee/norm);if(unequal)throw std::runtime_error("exact path changed FP32 results");
  auto time=[&](auto fn){hipEvent_t s,e;CK(hipEventCreate(&s));CK(hipEventCreate(&e));fn();CK(hipEventRecord(s));for(int i=0;i<reps;i++)fn();CK(hipEventRecord(e));CK(hipEventSynchronize(e));float ms;CK(hipEventElapsedTime(&ms,s,e));CK(hipEventDestroy(s));CK(hipEventDestroy(e));return ms/reps;};
  float rt=time(ref),ft=time(fast),mt=time(mv),et=time(exact);
  std::cout<<"P="<<P<<" E="<<E<<" D="<<D<<" M="<<M<<" relative_L2="<<rel<<" mv_L2="<<mrel<<" ref_ms="<<rt<<" wmma_ms="<<ft<<" mv_ms="<<mt<<" speedup="<<rt/ft<<std::endl;
  std::cout<<"exact_L2="<<erel<<" unequal="<<unequal<<" exact_ms="<<et<<" exact_speedup="<<rt/et<<std::endl;
  auto bench_variant=[&](auto pr,auto bn){
    auto fn=[&]{variant(pr,bn);};fn();CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*4,hipMemcpyDeviceToHost));
    double sum=0;for(size_t i=0;i<got.size();i++){if(!std::isfinite(got[i]))throw std::runtime_error("variant nonfinite");sum+=(got[i]-base[i])*(got[i]-base[i]);}
    double rel=sqrt(sum/norm);if(rel>(decltype(pr)::value?3e-5:.001))throw std::runtime_error("variant error");
    float ms=time(fn);std::cout<<"variant P="<<P<<" precise="<<decltype(pr)::value<<" BN="<<decltype(bn)::value<<" L2="<<rel<<" ms="<<ms<<std::endl;
  };
  if(E==512 && P<=16){
    auto fn=[&]{
      hgnv2gpu::rotate_fast<<<dim3(D/128,P),128>>>(dx,xr,su,D,false);
      hgnv2gpu::expert_mv_exact_packed<<<dim3((2*M+7)/8,S),256>>>(q,gs,xr,gu,di,D,2*M,K,S,false);
      hgnv2gpu::activate_rotate<<<dim3(M/128,S),128>>>(gu,hid,sv,du,M);
      hgnv2gpu::expert_mv_exact_packed<<<dim3((D+7)/8,S),256>>>(qd,ds,hid,pairs,di,M,D,K,S,true);
      hgnv2gpu::rotate_fast<<<dim3(D/128,S),128>>>(pairs,pairs,dv,D,true);
      hgnv2gpu::reduce<<<dim3((D+255)/256,P),256>>>(pairs,dw,out,D,K);
    };
    fn();CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*4,hipMemcpyDeviceToHost));
    for(size_t i=0;i<got.size();i++)if(got[i]!=base[i])throw std::runtime_error("packed MV exact mismatch");
    std::cout<<"exact_mv P="<<P<<" ms="<<time(fn)<<std::endl;
  }
  if(E==512 && P>=64){
    bench_variant(std::true_type{},std::integral_constant<int,32>{});
    bench_variant(std::false_type{},std::integral_constant<int,64>{});
    bench_variant(std::false_type{},std::integral_constant<int,32>{});
    bench_variant(std::false_type{},std::integral_constant<int,16>{});
  }
}
int main()try{
  for(int p:{1,17,65})test(p,19,256,384,3);
  for(int p:{1,4,16,64,256,1024})test(p,512,2560,640,3);
  std::cout<<"ALL PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
