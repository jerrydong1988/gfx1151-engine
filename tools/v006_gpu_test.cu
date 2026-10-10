// Directly exercise the production router and image-buffer growth code.
#define main gdec_real_main
#include "gdec.cpp"
#undef main
#include <random>

int main() {
  int failed = 0, cases = 0;
  for (int count : {1, 16, 17, 20, 32, 33}) {
    std::istringstream request("1 1 0 1 1 0 VIMG " + std::to_string(count));
    auto parsed = serve::parse_gen(request);
    const bool ok = parsed.vimg == count && parsed.reject == (count > 32);
    ++cases; failed += !ok;
    printf("%s VIMG count=%d reject=%d\n", ok ? "PASS" : "FAIL", count, parsed.reject);
  }
  std::mt19937 rng(61006);
  for (int P : {1, 2, 7, 8, 9, 33, 257}) {
    for (int pattern = 0; pattern < 6; ++pattern) {
      std::vector<float> x(P * 512);
      for (size_t i = 0; i < x.size(); ++i) {
        if (pattern == 0) x[i] = std::uniform_real_distribution<float>(-20, 20)(rng);
        if (pattern == 1) x[i] = 0;  // all tied; the old tree is NOT index order
        if (pattern == 2) x[i] = float(rng() % 7) - 3;
        if (pattern == 3) x[i] = (i % 512 == 3 || i % 512 == 32) ? 9.f : -9.f;
        if (pattern == 4) x[i] = (i % 17) ? -1.e20f : 1.e20f;
        if (pattern == 5) x[i] = std::nextafter(1.f, (i % 3) ? 2.f : 0.f);
      }
      float *dx, *wa, *wb; int *ia, *ib;
      CK(hipMalloc(&dx, x.size() * 4));
      CK(hipMalloc(&wa, (P * 16 + 32) * 4)); CK(hipMalloc(&wb, (P * 16 + 32) * 4));
      CK(hipMalloc(&ia, (P * 16 + 32) * 4)); CK(hipMalloc(&ib, (P * 16 + 32) * 4));
      CK(hipMemcpy(dx, x.data(), x.size()*4, hipMemcpyHostToDevice));
      for (void* ptr : {(void*)wa, (void*)wb, (void*)ia, (void*)ib})
        CK(hipMemset(ptr, 0xa5, (P*16+32)*4));
      k_router_topk<<<P,512>>>(dx, ia+16, wa+16, 512, 10);
      k_router_topk_w32<10><<<(P+7)/8,256>>>(dx, ib+16, wb+16, P);
      CK(hipDeviceSynchronize());
      std::vector<uint32_t> a(P*16+32),b(P*16+32);
      int diff=0;
      for (auto pair : {std::make_pair((void*)ia,(void*)ib), std::make_pair((void*)wa,(void*)wb)}) {
        CK(hipMemcpy(a.data(),pair.first,a.size()*4,hipMemcpyDeviceToHost));
        CK(hipMemcpy(b.data(),pair.second,b.size()*4,hipMemcpyDeviceToHost));
        for (size_t i=0;i<a.size();++i) diff += a[i]!=b[i];
        for (int i=0;i<16;++i) if(a[i]!=0xa5a5a5a5 || b[i]!=0xa5a5a5a5 ||
            a[a.size()-1-i]!=0xa5a5a5a5 || b[b.size()-1-i]!=0xa5a5a5a5) ++diff;
      }
      ++cases; failed += diff!=0;
      printf("%s router P=%d pattern=%d differing_words=%d\n",diff?"FAIL":"PASS",P,pattern,diff);
      CK(hipFree(dx));CK(hipFree(wa));CK(hipFree(wb));CK(hipFree(ia));CK(hipFree(ib));
    }
  }
  uint16_t* images=nullptr;size_t cap=0;
  grow_image_embeddings(images,cap,0,2000);
  std::vector<uint16_t> data(4500*2560),out(data.size());
  for(size_t i=0;i<data.size();++i) data[i]=uint16_t(i*137+19);
  CK(hipMemcpy(images,data.data(),2000*2560*2,hipMemcpyHostToDevice));
  grow_image_embeddings(images,cap,2000,2500);
  CK(hipMemcpy(out.data(),images,2000*2560*2,hipMemcpyDeviceToHost));
  bool first=cap>=4500 && std::equal(out.begin(),out.begin()+2000*2560,data.begin());
  CK(hipMemcpy(images,data.data(),data.size()*2,hipMemcpyHostToDevice));
  grow_image_embeddings(images,cap,4500,17);
  CK(hipMemcpy(out.data(),images,out.size()*2,hipMemcpyDeviceToHost));
  bool second=out==data && cap>=4517;
  auto same=images;grow_image_embeddings(images,cap,1,1);
  bool no_growth=same==images;
  CK(hipFree(images));
  failed+=!first+!second+!no_growth;cases+=3;
  printf("image_growth first=%d second=%d no_growth=%d\n",first,second,no_growth);
  printf("RESULT %s cases=%d failed=%d\n",failed?"FAIL":"PASS",cases,failed);
  return failed?1:0;
}
