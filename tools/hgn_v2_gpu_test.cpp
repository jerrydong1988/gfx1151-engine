// HIP/CPU differential tests; no model downloads needed.
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include "../src/hgn_v2.h"
#include "../src/gpu/parts/28_kernels_hgn_v2.inc"
#include <iostream>
#include <algorithm>
#include <random>

#define CK(e) do{auto s=(e);if(s!=hipSuccess)throw std::runtime_error(hipGetErrorString(s));}while(0)
template<class T> T* upload(const std::vector<T>& v){T* p;CK(hipMalloc(&p,v.size()*sizeof(T)));CK(hipMemcpy(p,v.data(),v.size()*sizeof(T),hipMemcpyHostToDevice));return p;}
static void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
__global__ void all_codebook(float* out){unsigned i=blockIdx.x*blockDim.x+threadIdx.x;out[i]=hgnv2gpu::cb(uint16_t(i));}
static void codebook(){
  std::vector<float> a(65536);auto p=upload(a);
  all_codebook<<<256,256>>>(p);CK(hipDeviceSynchronize());
  CK(hipMemcpy(a.data(),p,a.size()*4,hipMemcpyDeviceToHost));
  for(int i=0;i<65536;i++)check(a[i]==hgn_v2::trellis_value(uint16_t(i)),"codebook CPU/GPU mismatch");
  CK(hipFree(p));std::cout<<"65536 codebook states match exactly\n";
}
static void dense(uint32_t type,int n,int k){
  size_t nb=type==16?size_t(n)*k/2:size_t(n)*((k*13/16+15)&~15);
  std::vector<uint8_t> raw(nb);std::mt19937 rng(29+type);
  for(auto& b:raw)b=uint8_t(rng());
  std::vector<uint16_t> su(k),sv(n);
  for(int i=0;i<k;i++)su[i]=i%3?0x3c00:0xbc00;
  for(int i=0;i<n;i++)sv[i]=i%7?0x2800:0xa800;
  if(type==24)for(int r=0;r<n;r++)for(int g=0;g<k/64;g++){
    uint16_t sm[]={uint16_t(g%2?0xa400:0x2400),0x3400};
    memcpy(raw.data()+r*((k*13/16+15)&~15)+k*3/4+g*4,sm,4);
  }
  std::vector<float> ref(size_t(n)*k);std::vector<uint16_t> got(ref.size());
  hgn_v2::dequant(type,type==16?0x1208:64,raw.data(),n,k,nb,(uint8_t*)su.data(),(uint8_t*)sv.data(),n,0,n,ref.data());
  auto p=upload(raw);auto a=upload(su);auto b=upload(sv);auto out=upload(got);
  if(type==16)hgnv2gpu::ht_dense<<<dim3(k/128,n/128),256>>>((uint32_t*)p,(__half*)a,(__half*)b,out,k);
  else hgnv2gpu::q6_dense<<<(unsigned)((ref.size()+255)/256),256>>>(p,out,ref.size(),k);
  CK(hipGetLastError());CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),out,got.size()*2,hipMemcpyDeviceToHost));
  double err=0,norm=0;for(size_t i=0;i<ref.size();i++){uint32_t bits=uint32_t(got[i])<<16;float g;memcpy(&g,&bits,4);err+=(g-ref[i])*(g-ref[i]);norm+=ref[i]*ref[i];}
  double rel=std::sqrt(err/norm);std::cout<<"dense "<<type<<" "<<n<<"x"<<k<<" BF16 relative L2="<<rel<<"\n";check(rel<.003,"dense differential mismatch");
  CK(hipFree(p));CK(hipFree(a));CK(hipFree(b));CK(hipFree(out));
}
static void expert(bool per_slot){
  const int E=3,N=256,K=256,T=2,P=2,S=P*T;
  std::vector<uint8_t> raw(size_t(E)*N*K/2+size_t(E)*N*K/64);
  std::mt19937 rng(91);for(size_t i=0;i<size_t(E)*N*K/2;i++)raw[i]=uint8_t(rng());
  for(size_t i=size_t(E)*N*K/2;i<raw.size();i+=2){uint16_t s=(i%6)?0x2000:0xa000;memcpy(raw.data()+i,&s,2);}
  std::vector<uint16_t> su(K),sv(N);for(int c=0;c<K;c++)su[c]=c%3?0x3c00:0xbc00;for(int r=0;r<N;r++)sv[r]=r%5?0xbc00:0x3c00;
  std::vector<float> ref(size_t(E)*N*K);hgn_v2::dequant(23,128,raw.data(),E*N,K,raw.size(),(uint8_t*)su.data(),(uint8_t*)sv.data(),N,0,E*N,ref.data());
  const int XR=per_slot?S:P;
  std::vector<float> x(XR*K);for(auto& a:x)a=float(int(rng()%100)-50)/50;
  std::vector<int> ids(P*16,0);ids[0]=2;ids[1]=0;ids[16]=1;ids[17]=2;
  auto q=upload(raw);auto a=upload(su);auto b=upload(sv);auto dx=upload(x);auto ix=upload(ids);
  std::vector<float> got(S*N);auto y=upload(got);auto xr=upload(x);
  hgnv2gpu::rotate<<<dim3(K/128,XR),128>>>(dx,xr,(__half*)a,K,XR,false);
  hgnv2gpu::expert_mv<<<dim3((N+7)/8,S),256>>>(q,(__half*)(q+size_t(E)*N*K/2),xr,y,ix,K,N,T,S,per_slot);
  hgnv2gpu::rotate<<<dim3(N/128,S),128>>>(y,y,(__half*)b,N,S,true);
  CK(hipGetLastError());CK(hipDeviceSynchronize());CK(hipMemcpy(got.data(),y,got.size()*4,hipMemcpyDeviceToHost));
  double err=0,norm=0;for(int s=0;s<S;s++)for(int r=0;r<N;r++){
    double z=0;for(int c=0;c<K;c++)z+=double(ref[(size_t(ids[(s/T)*16+s%T])*N+r)*K+c])*x[(per_slot?s:s/T)*K+c];
    err+=(got[s*N+r]-z)*(got[s*N+r]-z);norm+=z*z;
  }
  double rel=std::sqrt(err/norm);std::cout<<"rotated expert GEMV per_slot="<<per_slot<<" relative L2="<<rel<<"\n";check(rel<1e-5,"expert differential mismatch");
  std::vector<float> weights(P*16,0),sum(P*N);
  for(int t=0;t<P;t++){weights[t*16]=.25f;weights[t*16+1]=.75f;}
  auto dw=upload(weights),dy=upload(sum);
  hgnv2gpu::reduce<<<dim3((N+255)/256,P),256>>>(y,dw,dy,N,T);
  CK(hipDeviceSynchronize());CK(hipMemcpy(sum.data(),dy,sum.size()*4,hipMemcpyDeviceToHost));
  for(int t=0;t<P;t++)for(int r=0;r<N;r++)check(std::fabs(sum[t*N+r]-(got[(t*T)*N+r]*.25f+got[(t*T+1)*N+r]*.75f))<1e-5,"weighted reduction");
  CK(hipFree(dw));CK(hipFree(dy));
  CK(hipFree(q));CK(hipFree(a));CK(hipFree(b));CK(hipFree(dx));CK(hipFree(ix));CK(hipFree(y));CK(hipFree(xr));
}
int main()try{codebook();dense(16,256,384);dense(24,7,320);expert(false);expert(true);std::cout<<"GPU differential tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
