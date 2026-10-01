#define main gdec_real_main
#include "gdec.cpp"
#undef main

#include <random>
#include <numeric>
#include <cmath>
#define PCHECK(x) do { auto e=(x); if(e!=hipSuccess) { fprintf(stderr,"HIP failure line %d: %s\n",__LINE__,hipGetErrorString(e)); exit(2); } } while(0)
template<class T>T* up(const std::vector<T>&v){T*d=nullptr;PCHECK(hipMalloc(&d,v.size()*sizeof(T)));PCHECK(hipMemcpy(d,v.data(),v.size()*sizeof(T),hipMemcpyHostToDevice));return d;}
template<class T>bool same(T*d,const std::vector<T>&v){std::vector<T>h(v.size());PCHECK(hipMemcpy(h.data(),d,v.size()*sizeof(T),hipMemcpyDeviceToHost));return !memcmp(h.data(),v.data(),v.size()*sizeof(T));}
int main(){
 hipStream_t st;PCHECK(hipStreamCreate(&st));hipEvent_t e0,e1;PCHECK(hipEventCreate(&e0));PCHECK(hipEventCreate(&e1));std::mt19937 rng(1717);int ci=0,fail=0;
 for(int perm:{0,1})for(int P:{1,3,4,5,255,256,257,1025,8192})for(int edge=0;edge<4;++edge){
  int base=256+edge,np=(base+P+255)/256,nphys=np+3;std::vector<int>pt(np);for(int i=0;i<np;++i)pt[i]=perm?(i*7)%nphys:i;
  // Use a true permutation for every page count, with noncontiguous holes.
  if(perm){std::vector<int>pool(nphys);std::iota(pool.begin(),pool.end(),0);std::shuffle(pool.begin(),pool.end(),rng);std::copy(pool.begin(),pool.begin()+np,pt.begin());}
  size_t n=(size_t)P*512,cache=(size_t)nphys*256*512,guard=64;
  std::vector<float>kb(n),vb(n),nw(256);std::vector<float2>cs((size_t)P*32);
  for(auto&v:kb)v=(int(rng()%2001)-1000)*.001f;for(auto&v:vb)v=(int(rng()%2001)-1000)*.01f;for(auto&v:nw)v=(int(rng()%201)-100)*.001f;
  for(auto&v:cs){float a=(rng()%1000)*.01f;v.x=cosf(a);v.y=sinf(a);}
  auto*dk=up(kb);auto*dv=up(vb);auto*dw=up(nw);auto*dc=up(cs);auto*dp=up(pt);
  std::vector<uint32_t>raw(n+2*guard,0x7fc00001);std::vector<uint16_t>init(cache+2*guard,0x3e23);
  uint32_t*ko[2];uint16_t*kc[2],*vc[2],*vt[2];for(int m=0;m<2;++m){ko[m]=up(raw);kc[m]=up(init);vc[m]=up(init);vt[m]=up(init);}
  struct Run{int lr,pr,n;};std::vector<Run>runs;
  for(int t=base;t<base+P;){int lp=t/256,pp=pt[lp],q=lp+1;while(q<np&&pt[q]==pp+q-lp)++q;int end=std::min(base+P,q*256);runs.push_back({t,pp*256+t%256,end-t});t=end;}
  auto launch=[&](int m){auto*kr=(float*)(ko[m]+guard);auto*k=kc[m]+guard,*v=vc[m]+guard,*tv=vt[m]+guard;
   if(m==0){k_qsa_kprep<false><<<dim3(2,P),256,0,st>>>(dk,dw,kr,nullptr,1e-6f,base,dc);
    for(auto r:runs){size_t so=(size_t)(r.lr-base)*512;unsigned g=(r.n*128+255)/256;
     k_f32_to_bf16_v4<<<g,256,0,st>>>(kr+so,k+(size_t)r.pr*512,512,r.n,512);
     k_f32_to_bf16_v4<<<g,256,0,st>>>(dv+so,v+(size_t)r.pr*512,512,r.n,512);
     unsigned bt=((r.pr+r.n+3)/4-r.pr/4)*512;k_f32_to_bf16_v4_bt<<<(bt+255)/256,256,0,st>>>(dv+so,tv,512,r.n,512,r.pr);
    }
   }else{k_qsa_kprep<true, true><<<dim3(2,P),256,0,st>>>(dk,dw,kr,k,1e-6f,base,dc,dp);
    unsigned bt=((base+P+3)/4-base/4)*512;k_f32_to_bf16_v4_dual<<<(bt+255)/256,256,0,st>>>(dv,tv,512,P,512,base,v,dp);
   }PCHECK(hipGetLastError());
  };
  launch(0);launch(1);PCHECK(hipStreamSynchronize(st));bool eq=true,guards=true,finite=true;
  std::vector<uint16_t>h0(init.size()),h1(init.size());for(int c=0;c<3;++c){auto*a=c==0?kc:c==1?vc:vt;PCHECK(hipMemcpy(h0.data(),a[0],init.size()*2,hipMemcpyDeviceToHost));PCHECK(hipMemcpy(h1.data(),a[1],init.size()*2,hipMemcpyDeviceToHost));eq &= h0==h1;for(size_t i=0;i<guard;++i)guards &=h0[i]==init[i]&&h1[i]==init[i]&&h0[cache+guard+i]==init[i]&&h1[cache+guard+i]==init[i];}
  std::vector<uint32_t>r0(raw.size()),r1(raw.size());PCHECK(hipMemcpy(r0.data(),ko[0],raw.size()*4,hipMemcpyDeviceToHost));PCHECK(hipMemcpy(r1.data(),ko[1],raw.size()*4,hipMemcpyDeviceToHost));eq &= r0==r1;
  for(size_t i=0;i<raw.size();++i)if(i<guard||i>=n+guard)guards &=r0[i]==raw[i]&&r1[i]==raw[i];else finite &=(r0[i]&0x7f800000)!=0x7f800000;
  bool ro=same(dk,kb)&&same(dv,vb)&&same(dw,nw)&&same(dc,cs)&&same(dp,pt);bool ok=eq&&guards&&finite&&ro;fail+=!ok;
  printf("{\"case\":%d,\"P\":%d,\"base\":%d,\"permuted\":%d,\"runs\":%zu,\"bitwise_equal\":%s,\"guards\":%s,\"finite\":%s,\"inputs_unchanged\":%s}\n",ci,P,base,perm,runs.size(),eq?"true":"false",guards?"true":"false",finite?"true":"false",ro?"true":"false");
  if(ok&&edge==0&&P>=1025)for(int b=0;b<10;++b)for(int j=0;j<2;++j){int m=(j+b)%2;PCHECK(hipEventRecord(e0,st));for(int r=0;r<10;++r)launch(m);PCHECK(hipEventRecord(e1,st));PCHECK(hipEventSynchronize(e1));float ms=0;PCHECK(hipEventElapsedTime(&ms,e0,e1));printf("{\"case\":%d,\"P\":%d,\"block\":%d,\"mode\":%d,\"ms\":%.9f}\n",ci,P,b,m,ms/10);}
  for(int m=0;m<2;++m){PCHECK(hipFree(ko[m]));PCHECK(hipFree(kc[m]));PCHECK(hipFree(vc[m]));PCHECK(hipFree(vt[m]));}PCHECK(hipFree(dk));PCHECK(hipFree(dv));PCHECK(hipFree(dw));PCHECK(hipFree(dc));PCHECK(hipFree(dp));++ci;
 }
 printf("{\"failures\":%d}\n",fail);return fail?1:0;
}
