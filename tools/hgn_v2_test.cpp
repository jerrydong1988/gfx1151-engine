// CPU format tests, plus optional read-only validation of private model files.
// clang++ -std=c++17 -O2 tools/hgn_v2_test.cpp -o build/hgn-v2-test
#include "../src/hgn.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>

static void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
int main(int argc,char** argv) try {
  float x[128];for(int j=0;j<128;j++)x[j]=float(j-64)/100;
  hgn_v2::had128(x);hgn_v2::had128(x);
  for(int j=0;j<128;j++)check(std::fabs(x[j]-float(j-64)/100)<1e-6,"Hadamard involution");
  check(hgn_v2::trellis_value(0)==-3.453125f,"codebook state 0");
  // High/low code planes, row padding, signed scale and nonzero offset.
  const int K=320,R=3,stride=272;std::vector<uint8_t> q(R*stride);
  for(int r=0;r<R;r++){
    for(int c=0;c<K;c++){int v=(c+7*r)%64;q[r*stride+c/2]|=(v&15)<<((c%2)*4);q[r*stride+K/2+c/4]|=(v>>4)<<((c%4)*2);}
    for(int g=0;g<K/64;g++){uint16_t sm[]={0xb800,0x3c00};memcpy(q.data()+r*stride+K*3/4+g*4,sm,4);}
  }
  std::vector<float> a(R*K);hgn_v2::dequant(24,64,q.data(),R,K,q.size(),nullptr,nullptr,R,0,R,a.data());
  for(int r=0;r<R;r++)for(int c=0;c<K;c++)check(a[r*K+c]==1.f-.5f*((c+7*r)%64),"q6 code plane/padding");
  bool rejected=false;try{hgn_v2::validate(16,0x1209,128,128,8192);}catch(...){rejected=true;}
  check(rejected,"unknown HT variant must fail");
  std::cout<<"CPU synthetic format tests passed\n";
  if(argc<2)return 0;
  hgn::Checkpoint ck(argv[1]);std::unique_ptr<hgn::Checkpoint> old;
  for(int i=2;i<argc;i++) {
    if(std::string(argv[i])=="--compare-v1" && i+1<argc)old=std::make_unique<hgn::Checkpoint>(argv[++i]);
    else ck.add_overlay(argv[i]);
  }
  ck.validate_v2();int count=0;double maxval=0;
  double mincos=1,avgcos=0;int ncmp=0;std::string worst;
  for(const auto& kv:ck.tensors()){
    const auto& t=kv.second;if(t.dtype!=16&&t.dtype!=23&&t.dtype!=24)continue;
    auto rot=ck.rotation(t);size_t cols=t.dims[t.ndims-1],rows=t.numel()/cols;
    size_t nr=t.dtype==24?std::min<size_t>(rows,4):128;
    a.resize(nr*cols);
    hgn_v2::dequant(t.dtype,t.qparam,t.data,rows,cols,t.data_size,
      rot.first?rot.first->data:nullptr,rot.second?rot.second->data:nullptr,t.dims[t.ndims-2],0,nr,a.data());
    for(float f:a){check(std::isfinite(f),"nonfinite decoded weight");maxval=std::max(maxval,double(std::fabs(f)));}
    if(old) {
      const auto* prev=old->find(t.name);
      if(prev && prev->dtype==5 && prev->numel()==t.numel()) {
        auto q=hgn::Checkpoint::q4cp_parse(*prev);std::vector<float> row(cols);
        double aa=0,bb=0,ab=0;
        for(size_t r=0;r<nr;r++) {
          hgn::Checkpoint::q4cp_row(q,r,row.data());
          for(size_t c=0;c<cols;c++){double u=a[r*cols+c],v=row[c];aa+=u*u;bb+=v*v;ab+=u*v;}
        }
        double co=ab/std::sqrt(aa*bb);avgcos+=co;ncmp++;
        if(co<mincos){mincos=co;worst=t.name;}
      }
    }
    count++;
  }
  std::cout<<"validated all v2 metadata and sampled "<<count<<" decoded matrices; max_abs="<<maxval<<"\n";
  if(ncmp)std::cout<<"v1 Q4 comparison (not a full precision oracle): "<<ncmp<<" samples, mean cosine="<<avgcos/ncmp<<", minimum="<<mincos<<" ("<<worst<<")\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
