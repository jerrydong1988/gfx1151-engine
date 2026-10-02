// Independent CPU dequantization oracle for the optional upstream HT backend.
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include "../src/hgn_v2.h"
__device__ __forceinline__ uint16_t f2bf(float x) {
  unsigned u=__float_as_uint(x);return uint16_t((u+0x7fffu+((u>>16)&1u))>>16);
}
#include "../src/gpu/parts/28_kernels_hgn_v2_upstream.inc"
#include <algorithm>
#include <iostream>
#include <random>
#include <map>
#include <tuple>
#include <stdexcept>
#include <vector>
#define CHECK_HIP(e) do{auto s=(e);if(s!=hipSuccess)throw std::runtime_error(hipGetErrorString(s));}while(0)
template<class T> T* upload(const std::vector<T>& v) {
  T* p;CHECK_HIP(hipMalloc(&p,v.size()*sizeof(T)));
  CHECK_HIP(hipMemcpy(p,v.data(),v.size()*sizeof(T),hipMemcpyHostToDevice));return p;
}
static std::map<std::tuple<int,int,bool>,std::vector<float>> ordered_serial;
template<int P, bool Ordered, bool Bf16=false> void verify(int N,int K) {
  std::mt19937 rng(319+N+K);std::vector<uint8_t> codes(size_t(N)*K/2);
  for(auto&v:codes)v=uint8_t(rng());
  std::vector<uint16_t> suh(K),svh(N);std::vector<float> su(K),sv(N);
  for(int i=0;i<K;i++) {suh[i]=i%3?0x3c00:0xbc00;su[i]=i%3?1.f:-1.f;}
  for(int i=0;i<N;i++) {svh[i]=i%5?0x2800:0xa800;sv[i]=i%5?0.03125f:-0.03125f;}
  std::vector<float> weights(size_t(N)*K),x(size_t(P)*K),out(size_t(P)*N),scratch(size_t(P)*N*80);
  std::vector<int> count(N/128);
  hgn_v2::dequant(16,0x1208,codes.data(),N,K,codes.size(),(uint8_t*)suh.data(),(uint8_t*)svh.data(),N,0,N,weights.data());
  for(auto&v:x)v=float(int(rng()%2001)-1000)*0.002f;
  std::vector<uint16_t> xbf(x.size());
  if constexpr (Bf16) for(size_t i=0;i<x.size();i++) {
    uint32_t u;memcpy(&u,&x[i],4);u=(u+0x7fffu+((u>>16)&1u))&0xffff0000u;
    xbf[i]=uint16_t(u>>16);memcpy(&x[i],&u,4);
  }
  std::vector<double> ref(size_t(P)*N);
  for(int p=0;p<P;p++)for(int r=0;r<N;r++)for(int c=0;c<K;c++)ref[p*N+r]+=double(weights[size_t(r)*K+c])*x[p*K+c];
  auto dc=upload(codes);auto ds=upload(su);auto dv=upload(sv);auto dx=upload(x);auto db=upload(xbf);auto dy=upload(out);auto dz=upload(scratch);auto dn=upload(count);
  const int ntx=K/128,nty=N/128;
  int ks=std::max(1,std::min((80+nty-1)/nty,ntx));const int tpb=(ntx+ks-1)/ks;ks=(ntx+tpb-1)/tpb;
  const int tpc=std::min(tpb,std::max(1,24/P));size_t lds=size_t(std::max(P*tpc,8*P))*128*4;
  std::vector<float> first;size_t differences=0;double worst=0;
  for(int repeat=0;repeat<8;repeat++) {
    if constexpr (Bf16)
      k_ht_gemv<P,uint16_t,Ordered><<<dim3(nty,ks),256,lds>>>(dc,ds,dv,db,K,dy,N,nty*128,K,N,tpb,tpc,dz,dn);
    else
      k_ht_gemv<P,float,Ordered><<<dim3(nty,ks),256,lds>>>(dc,ds,dv,dx,K,dy,N,nty*128,K,N,tpb,tpc,dz,dn);
    CHECK_HIP(hipDeviceSynchronize());CHECK_HIP(hipMemcpy(out.data(),dy,out.size()*4,hipMemcpyDeviceToHost));
    double er=0,norm=0;for(size_t i=0;i<out.size();i++){double e=out[i]-ref[i];er+=e*e;norm+=ref[i]*ref[i];if(repeat && memcmp(&out[i],&first[i],4))differences++;}
    double rel=std::sqrt(er/norm);worst=std::max(worst,rel);
    if(!std::isfinite(rel)||rel>2e-6)throw std::runtime_error("upstream HT differs from CPU oracle");
    if(!repeat)first=out;
  }
  if(Ordered && differences)throw std::runtime_error("ordered HT repeat mismatch");
  if constexpr (Ordered) {
    if constexpr (P==1) ordered_serial[{N,K,Bf16}]=first;
    else {
      const auto& serial=ordered_serial.at({N,K,Bf16});
      if(memcmp(serial.data(),first.data(),N*4))throw std::runtime_error("ordered HT first row differs between serial and multi-row");
    }
  }
  std::cout<<"upstream HT "<<N<<"x"<<K<<" P="<<P<<" bf16="<<Bf16<<" ordered="<<Ordered<<" split="<<ks<<" worst_relative_l2="<<worst<<" repeat_bit_differences="<<differences<<"/"<<out.size()*7<<"\n";
  CHECK_HIP(hipFree(dc));CHECK_HIP(hipFree(ds));CHECK_HIP(hipFree(dv));CHECK_HIP(hipFree(dx));CHECK_HIP(hipFree(db));CHECK_HIP(hipFree(dy));CHECK_HIP(hipFree(dz));CHECK_HIP(hipFree(dn));
}
template<bool Bf16> void ordered_suite() {
  verify<1,true,Bf16>(256,384);verify<3,true,Bf16>(256,384);
  verify<1,true,Bf16>(1024,2560);verify<2,true,Bf16>(1024,2560);
  verify<3,true,Bf16>(1024,2560);verify<4,true,Bf16>(1024,2560);
  verify<5,true,Bf16>(1024,2560);verify<6,true,Bf16>(1024,2560);
  verify<7,true,Bf16>(1024,2560);verify<8,true,Bf16>(1024,2560);
  // No split and a wider projection; exercise the 24/P staging boundary.
  verify<1,true,Bf16>(128,128);verify<8,true,Bf16>(128,128);
  verify<1,true,Bf16>(256,10240);verify<8,true,Bf16>(256,10240);
}
int main()try {
  verify<1,false>(256,384);verify<3,false>(256,384);verify<1,false>(1024,2560);verify<3,false>(1024,2560);
  ordered_suite<false>();ordered_suite<true>();
  std::cout<<"PASS optional upstream HT versus independent FP32 decoded weight / FP64 dot oracle\n";
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
