// Test glue only: generated .inc files contain verbatim engine implementations.
// --cpu-only substitutes host memory for upload transport, never launches HIP.
#include <hip/hip_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#define CK(e) do { hipError_t rc=(e); if(rc!=hipSuccess) throw std::runtime_error(std::string(#e)+": "+hipGetErrorString(rc)); } while(0)
#define DALLOC(p,n) CK(hipMalloc((void**)(p),(n)))
hipStream_t g_str=nullptr;
struct { int vocab=0; } g_cfg;
bool cpu_only=false;
hipError_t probe_copy(void* dst,const void* src,size_t bytes,hipMemcpyKind kind,hipStream_t stream) {
  if(cpu_only){std::memcpy(dst,src,bytes);return hipSuccess;}
  return hipMemcpyAsync(dst,src,bytes,kind,stream);
}
#include "gpu-kernels.inc"
#include "host-sampler.inc"
struct CorrectionOracle:HostSampler {
  void apply(std::vector<float>& logits) {
#include "host-correction-body.inc"
  }
};
struct Probe {
  static constexpr int SMS_ROWS=65,SMS_TCAP=512,SMS_PCAP=64;
  static constexpr int SMS_BIAS_CAP=20480,SMS_HIST_CAP=120000;
  int sm_nb=0,sm_nh=0;
  int *d_sm_bids=nullptr,*d_sm_hids=nullptr,*d_sm_pids=nullptr,*d_sm_pn=nullptr;
  float *d_sm_bvals=nullptr,*d_sm_hvals=nullptr,*d_sm_pvals=nullptr,*d_sm_scratch=nullptr;
  int *d_sm_ctrl=nullptr,*d_sm_cand_ids=nullptr,*d_sm_cand_n=nullptr;
  unsigned* d_sm_hist=nullptr;
  float* d_sm_cand_vals=nullptr;
  std::vector<int> h_sm_ci,h_sm_pi,h_sm_ids,sms_out_ids;
  std::vector<float> h_sm_cv,h_sm_pv,h_sm_vals,sms_out_vals;
  std::vector<void*> allocations;
  template<class T> void alloc(T*& p,size_t n) {
    if(cpu_only){p=(T*)std::calloc(n,sizeof(T));if(!p)throw std::bad_alloc();}
    else DALLOC(&p,n*sizeof(T));
    allocations.push_back(p);
  }
  Probe(){
    alloc(d_sm_bids,SMS_BIAS_CAP);alloc(d_sm_bvals,SMS_BIAS_CAP);
    alloc(d_sm_hids,SMS_HIST_CAP);alloc(d_sm_hvals,SMS_HIST_CAP);
    alloc(d_sm_pids,SMS_ROWS*SMS_PCAP);alloc(d_sm_pvals,SMS_ROWS*SMS_PCAP);alloc(d_sm_pn,SMS_ROWS);
    if(!cpu_only){
      alloc(d_sm_ctrl,SMS_ROWS*8);alloc(d_sm_hist,SMS_ROWS*4096);
      alloc(d_sm_cand_ids,SMS_ROWS*SMS_TCAP);alloc(d_sm_cand_vals,SMS_ROWS*SMS_TCAP);alloc(d_sm_cand_n,SMS_ROWS);
    }
    h_sm_ids.resize(SMS_ROWS*SMS_TCAP);h_sm_vals.resize(SMS_ROWS*SMS_TCAP);
  }
  ~Probe(){for(void* p:allocations){if(cpu_only)std::free(p);else hipFree(p);}if(d_sm_scratch)hipFree(d_sm_scratch);}
#define hipMemcpyAsync probe_copy
#include "round-upload.inc"
#include "prefix-upload.inc"
#undef hipMemcpyAsync
#include "gpu-dispatch.inc"
  template<class T> std::vector<T> read(T* p,size_t n){
    std::vector<T> v(n);
    if(cpu_only)std::memcpy(v.data(),p,n*sizeof(T));
    else CK(hipMemcpy(v.data(),p,n*sizeof(T),hipMemcpyDeviceToHost));
    return v;
  }
};
uint32_t bits(float x){uint32_t u;std::memcpy(&u,&x,4);return u;}
struct Case {
  std::string name; float presence,frequency; int offset,rows,pattern;
  bool base=true,bias=true,overflow=false,null_prefix=false;
};
HostSampler setup(const Case& c) {
  HostSampler s{};s.temp=.7f;s.top_p=1;s.min_p=0;s.top_k=20;s.rng=1234;
  s.presence=c.presence;s.frequency=c.frequency;
  if(c.base)s.hist={{1,7},{2,123},{5,1},{31,19},{130,2001}};
  if(c.bias)s.bias={{1,.13f},{2,-.27f},{5,1.07f},{31,-.11f},{200,.49f}};
  return s;
}
int run(Probe& p,const Case& c){
  const int n=c.overflow?4097:257;g_cfg.vocab=n;
  const auto s=setup(c);
  auto saved_pi=p.d_sm_pids;auto saved_pv=p.d_sm_pvals;auto saved_pn=p.d_sm_pn;
  if(c.null_prefix){p.d_sm_pids=nullptr;p.d_sm_pvals=nullptr;p.d_sm_pn=nullptr;}
  std::vector<int> draft(64);
  for(int i=0;i<64;i++)draft[i]=c.pattern==0?1:(c.pattern==1?(i%7==0?31:(i%3==0?2:5+i%11)):i+1);
  const bool shared=p.sms_round_shared(s);
  const bool prefix=p.sms_prefix_upload(s,draft.data(),c.offset,c.rows);
  if(!cpu_only)CK(hipStreamSynchronize(g_str));
  auto pi=prefix?p.read(p.d_sm_pids,c.rows*64):std::vector<int>();
  auto pv=prefix?p.read(p.d_sm_pvals,c.rows*64):std::vector<float>();
  auto pn=prefix?p.read(p.d_sm_pn,c.rows):std::vector<int>();
  auto hi=p.read(p.d_sm_hids,p.sm_nh);auto hv=p.read(p.d_sm_hvals,p.sm_nh);
  auto bi=p.read(p.d_sm_bids,p.sm_nb);auto bv=p.read(p.d_sm_bvals,p.sm_nb);
  std::mt19937 rng(99317);std::vector<float> input((size_t)c.rows*n);
  for(float& x:input)x=c.overflow?0.f:((int)(rng()%40001)-20000)*.0003f;
  std::vector<float> expected(input.size()),staged(input.size());
  int upload_bad=0,corrected_bad=0,ids_bad=0,probs_bad=0;bool pristine=true,gpu_ok=false;
  std::vector<HostSampler> views;
  for(int r=0;r<c.rows;r++){
    auto view=s;for(int j=0;j<c.offset+r;j++)view.remember(draft[j]);
    views.push_back(view);
    std::vector<float> row(input.begin()+(size_t)r*n,input.begin()+(size_t)(r+1)*n);
    CorrectionOracle oracle;static_cast<HostSampler&>(oracle)=view;oracle.apply(row);
    std::copy(row.begin(),row.end(),expected.begin()+(size_t)r*n);
    // Transport/uploader check only; CPU mode does not claim GPU coverage.
    std::vector<float> check(input.begin()+(size_t)r*n,input.begin()+(size_t)(r+1)*n);
    for(size_t j=0;j<bi.size();j++)check[bi[j]]+=bv[j];
    auto post_bias=check;
    for(size_t j=0;j<hi.size();j++)check[hi[j]]-=hv[j];
    if(prefix){
      std::vector<int> seen;
      for(int j=0;j<c.offset+r;j++)if(std::find(seen.begin(),seen.end(),draft[j])==seen.end())seen.push_back(draft[j]);
      if(pn[r]!=(int)seen.size())++upload_bad;
      for(int j=0;j<pn[r];j++){
        int id=pi[r*64+j];if(j>=(int)seen.size()||id!=seen[j])++upload_bad;
        check[id]=post_bias[id]-pv[r*64+j];
      }
    }
    for(int j=0;j<n;j++)if(bits(check[j])!=bits(row[j]))++upload_bad;
    std::copy(check.begin(),check.end(),staged.begin()+(size_t)r*n);
  }
  if(!cpu_only){
    float* x=nullptr;float* raw=nullptr;DALLOC(&x,input.size()*4);DALLOC(&raw,input.size()*4);
    CK(hipMemcpy(x,input.data(),input.size()*4,hipMemcpyHostToDevice));
    CK(hipMemcpy(raw,input.data(),input.size()*4,hipMemcpyHostToDevice));
    k_corr_apply<<<dim3(1,c.rows),256,0,g_str>>>(x,n,p.d_sm_bids,p.d_sm_bvals,p.sm_nb,p.d_sm_hids,p.d_sm_hvals,p.sm_nh,p.d_sm_pids,p.d_sm_pvals,prefix?p.d_sm_pn:nullptr,64);
    CK(hipGetLastError());CK(hipStreamSynchronize(g_str));
    auto actual=p.read(x,input.size());for(size_t i=0;i<actual.size();i++)if(bits(actual[i])!=bits(expected[i]))++corrected_bad;
    if(p.d_sm_scratch){CK(hipFree(p.d_sm_scratch));p.d_sm_scratch=nullptr;}
    std::vector<int> ids(c.rows*s.top_k);std::vector<float> vals(ids.size());
    gpu_ok=p.sms_topk_rows(raw,n,c.rows,s.top_k,prefix,ids.data(),vals.data());
    CK(hipGetLastError());CK(hipStreamSynchronize(g_str));
    auto raw_after=p.read(raw,input.size());pristine=std::memcmp(raw_after.data(),input.data(),input.size()*4)==0;
    if(gpu_ok){
      for(int r=0;r<c.rows;r++){
        auto view=views[r];auto dense=view;
        std::vector<float> probs(input.begin()+(size_t)r*n,input.begin()+(size_t)(r+1)*n);
        dense.prepare(probs);
        for(int j=0;j<s.top_k;j++){
          int id=ids[r*s.top_k+j];if(id!=dense.cand[j])++ids_bad;
          if(id<0||id>=n||bits(vals[r*s.top_k+j])!=bits(expected[(size_t)r*n+id]))++corrected_bad;
        }
        for(float top_p:{1.f,.65f,.8f})for(float min_p:{0.f,.1f}){
          auto hs=view;hs.top_p=top_p;hs.min_p=min_p;
          auto sp=hs;HostSampler::SDist sd;
          std::vector<float> target(input.begin()+(size_t)r*n,input.begin()+(size_t)(r+1)*n);hs.prepare(target);
          sp.prepare_sparse(ids.data()+r*s.top_k,vals.data()+r*s.top_k,s.top_k,sd);
          if(sd.ids!=hs.cand)++probs_bad;
          for(size_t j=0;j<sd.ids.size();j++)if(bits(sd.pr[j])!=bits(target[sd.ids[j]]))++probs_bad;
        }
      }
    }
    CK(hipFree(x));CK(hipFree(raw));
  }
  const bool pass=shared&&upload_bad==0&&corrected_bad==0&&ids_bad==0&&probs_bad==0&&pristine&&(cpu_only||gpu_ok!=c.overflow);
  p.d_sm_pids=saved_pi;p.d_sm_pvals=saved_pv;p.d_sm_pn=saved_pn;
  std::cout<<"{\"case\":\""<<c.name<<"\",\"gpu_executed\":"<<(!cpu_only?"true":"false")
    <<",\"rows\":"<<c.rows<<",\"base_off\":"<<c.offset<<",\"last_prefix\":"<<c.offset+c.rows-1
    <<",\"prefix_enabled\":"<<(prefix?"true":"false")<<",\"null_prefix_buffers\":"<<(c.null_prefix?"true":"false")<<",\"uploader_bit_mismatches\":"<<upload_bad
    <<",\"correction_bit_mismatches\":"<<corrected_bad<<",\"topk_id_mismatches\":"<<ids_bad
    <<",\"probability_bit_mismatches\":"<<probs_bad<<",\"topk_success\":"<<(gpu_ok?"true":"false")
    <<",\"expected_overflow\":"<<(c.overflow?"true":"false")<<",\"input_pristine\":"<<(pristine?"true":"false")
    <<",\"pass\":"<<(pass?"true":"false")<<"}\n";
  return pass?0:1;
}
int main(int argc,char** argv){try{
  std::string filter;
  for(int i=1;i<argc;i++){
    std::string a=argv[i];if(a=="--cpu-only")cpu_only=true;
    else if(a=="--case"&&i+1<argc)filter=argv[++i];
    else if(a=="--help"){std::cout<<"Prefix correction: --cpu-only (no GPU), --case NAME. Default explicitly runs GPU.\n";return 0;}
    else throw std::runtime_error("Unknown argument: "+a);
  }
  std::vector<Case> cases;
  cases.push_back({"first_bias_base_without_prefix_buffers",.1f,.3f,0,1,1,true,true,false,true});
  cases.push_back({"bias_only_without_prefix_buffers",0.f,0.f,0,1,1,false,true,false,true});
  for(int pattern:{0,1,2})for(int length:{1,8,64})for(int sign:{1,-1}){
    cases.push_back({"merged_p"+std::to_string(pattern)+"_len"+std::to_string(length)+"_sign"+std::to_string(sign),.13f*sign,.1f*sign,0,length+1,pattern});
  }
  cases.push_back({"new_ids_freq03",.17f,.3f,0,65,1,false,true});
  cases.push_back({"chunk_offset17",.19f,.3f,17,9,1});
  cases.push_back({"chunk_offset56_to64",-.13f,.1f,56,9,1});
  cases.push_back({"presence_only",.3f,0.f,0,65,1});
  cases.push_back({"frequency_only",0.f,-.3f,0,65,1});
  cases.push_back({"zero_penalties",0.f,0.f,0,65,1});
  cases.push_back({"no_prefix_row",.1f,.3f,0,1,1});
  cases.push_back({"overflow_fallback_prefix",.3f,.1f,0,9,1,true,false,true});
  Probe p;int failures=0,total=0;
  for(const auto& c:cases)if(filter.empty()||filter==c.name){failures+=run(p,c);++total;}
  std::cout<<"{\"summary\":true,\"gpu_executed\":"<<(!cpu_only?"true":"false")<<",\"cases\":"<<total<<",\"failures\":"<<failures<<"}\n";
  return total==0?2:failures?1:0;
}catch(const std::exception& e){std::cerr<<"PROBE_EXCEPTION "<<e.what()<<'\n';return 2;}}
