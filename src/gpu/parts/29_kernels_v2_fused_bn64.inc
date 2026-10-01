// R12c: independently validated BN64 candidate, isolated from the R12b kernel.
// R16 adds optional double buffering; the default template keeps the R12c loop.
// Production eligibility and mode selection are controlled by 40_model.inc.
namespace r12guv3 {
// Exact helper excerpt from r12-activate-wave4.cuh.
// Helper body UTF-8 SHA256: 1cb30d98453b2c33dccd41acf6679489447846b330e6d963a33a2ae2b6476f44
__device__ __forceinline__ void r12_had128_wave4(float (&v)[4]) {
  const int lane=threadIdx.x&31;
#pragma unroll
  for(int h=1;h<32;h*=2) {
#pragma unroll
    for(int i=0;i<4;i++) {
      const float b=__shfl_xor(v[i],h,32);
      v[i]=(lane&h)?b-v[i]:v[i]+b;
    }
  }
  // h=32: retain original b-v operand order for the upper half.
  const float a0=v[0]+v[1], a1=v[0]-v[1];
  const float a2=v[2]+v[3], a3=v[2]-v[3];
  // h=64, then exactly one normalization multiply per component.
  v[0]=(a0+a2)*0.08838834764831844f;
  v[1]=(a1+a3)*0.08838834764831844f;
  v[2]=(a0-a2)*0.08838834764831844f;
  v[3]=(a1-a3)*0.08838834764831844f;
}

// R12 v3 standalone candidate: BN64 / 512 threads with row-distributed waves.
// Production dispatch is guarded in 40_model.inc; the original staging is default.
// Requires moegg::{v16h,v8f,wmma}, moelut::{LutW,stage_sync}, and MoeTile.
// The embedded wave4 helper preserves had128 operations with new ownership.
// Launch: grid(mid/128, max_tiles, 1), block(512). mid,k are multiples of 128.
// input: FP16 [token][hi[k],lo[k]], output: FP16 [sorted slot][hi[mid],lo[mid]].
// Only HGNv2 precise Group128. Not the exact-FP32 or aligned-spec expert path.
// p.w/sc contain [expert][2*mid][k/2 or k/128*2], with gate then up.
// No GU global intermediate. Every floating-point dot/scale/activation/Had
// expression preserves the original grouped GU + activate_rotate expression.
// That is a hypothesis about bitwise output until verified on the target GPU.
template<bool LdsSync=false, bool DeferredScale=false, bool PermuteQ4=false, bool Double=false>
__launch_bounds__(512) __global__
void k_v2_fused_gu_rotate_r12(moelut::LutW p, const __half* __restrict__ input,
    const int* __restrict__ tokidx, const MoeTile* __restrict__ tiles,
    const int* __restrict__ ntiles, __half* __restrict__ hid,
    const __half* __restrict__ gu_sv, const __half* __restrict__ dn_su,
    int mid, int k) {
  using namespace moegg;
  constexpr int BM=256, BN=64, BK=2, kTokTiles=4;
  constexpr int kCodeBytes=BM*BK*16;
  constexpr int kScaleBytes=BK*BM*4;
  constexpr int kActStride=BN+1;
  constexpr int kActBytes=BK*4*kActStride*16;
  constexpr int kStageBytes=kCodeBytes+kScaleBytes+2*kActBytes;
  constexpr int kGuFloats=2*8*256; // gate/up, each 8 row waves; one 16-token tile
  constexpr int kHadFloats=0; // Had128 wave4 stays in registers
  static_assert(kStageBytes >= (kGuFloats+kHadFloats)*sizeof(float), "epilogue fits LDS");
  __shared__ __attribute__((aligned(16))) uint8_t lds[kStageBytes*(Double?2:1)];

  if((int)blockIdx.y>=*ntiles)return;
  MoeTile tile=tiles[blockIdx.y];
  const int start=(int)blockIdx.z*BN;
  if(start>=tile.count)return; // uniform CTA exit, before all barriers
  tile.first+=start; tile.count=min(BN,tile.count-start);
  const int slot0=tile.first,nrows=tile.count;
  const int live_tok_tiles=(nrows+15)/16;
  const int tid=threadIdx.x,wave_id=tid>>5,lane_id=tid&31;
  const int row_tile=wave_id>>3,row_wave=wave_id&7; // gate/up, then 128 rows
  const int sub_lane=lane_id&15,half_id=lane_id>>4;
  const int r_block=(int)blockIdx.x*128;
  const int num_kb=k/32, f_c=tid&1;
  const size_t rb=(size_t)k/2;
  // 512 threads stage each (row, BK column) exactly once. Physical weight
  // rows are two separately based 128-row blocks, also when mid is 640.
  const int stage_row=tid>>1;
  const size_t physical_row=(size_t)tile.expert*p.e_rows
      +(stage_row>>7)*mid+r_block+(stage_row&127);
  const uint8_t* f_ptr=p.w+physical_row*rb;
  const uint8_t* f_sp=p.sc+physical_row*(size_t)p.sstride;
  uint4 f_codes=make_uint4(0,0,0,0);
  uint32_t f_s=0;uint16_t f_s16=0;
  // BN*BK*4 == 512: one 8-half activation chunk per thread per stage.
  const int at=tid/(BK*4),asub=tid%(BK*4);
  const int src=at<nrows?(tokidx?tokidx[slot0+at]:slot0+at):-1;
  const __half* a_src=src>=0?input+(size_t)src*k*2+asub*8:nullptr;
  const int a_slot=asub*kActStride+at;
  uint4 a_data,a_low;
  const auto swizzle=[](int row,int c){return row*BK+(c^((row>>2)&1));};
  const auto fetch_stage=[&](int kb0){
    const int kb=kb0+f_c;
    f_codes=*reinterpret_cast<const uint4*>(f_ptr+(size_t)kb*16);
    if constexpr(DeferredScale) f_s16=reinterpret_cast<const uint16_t*>(f_sp)[kb/4];
    else f_s=reinterpret_cast<const uint16_t*>(f_sp)[kb/4];
    a_data=a_src?*reinterpret_cast<const uint4*>(a_src+kb0*32):make_uint4(0,0,0,0);
    a_low=a_src?*reinterpret_cast<const uint4*>(a_src+k+kb0*32):make_uint4(0,0,0,0);
  };
  const auto commit_stage=[&](int buf=0){
    auto* s_codes=reinterpret_cast<uint4*>((lds+buf*kStageBytes));
    auto* s_scale=reinterpret_cast<uint32_t*>((lds+buf*kStageBytes)+kCodeBytes);
    auto* s_act=reinterpret_cast<uint4*>((lds+buf*kStageBytes)+kCodeBytes+kScaleBytes);
    auto* s_act_lo=s_act+kActBytes/16;
    s_codes[swizzle(stage_row,f_c)]=f_codes;
    uint32_t s16;
    if constexpr(DeferredScale)s16=uint32_t(f_s16);else s16=f_s;
    s_scale[f_c*BM+stage_row]=s16|(s16<<16);
    s_act[a_slot]=a_data;s_act_lo[a_slot]=a_low;
  };
  // Four token fragments per lane, exactly the original BN64 fragment count.
  // Each wave now owns gate OR up; no dot-product reduction is split by wave.
  v8f acc[kTokTiles],group[kTokTiles],group_lo[kTokTiles];
#pragma unroll
  for(int j=0;j<kTokTiles;j++){acc[j]=v8f{};group[j]=v8f{};group_lo[j]=v8f{};}

  const auto compute_stage=[&](int kb0,int buf=0){
    auto* s_codes=reinterpret_cast<uint4*>((lds+buf*kStageBytes));
    auto* s_scale=reinterpret_cast<uint32_t*>((lds+buf*kStageBytes)+kCodeBytes);
    auto* s_act=reinterpret_cast<uint4*>((lds+buf*kStageBytes)+kCodeBytes+kScaleBytes);
    auto* s_act_lo=s_act+kActBytes/16;
      const int row=row_tile*128+row_wave*16+sub_lane;
      uint4 raw[BK];
#pragma unroll
      for(int c=0;c<BK;c++)raw[c]=s_codes[swizzle(row,c)];
#pragma unroll
      for(int kb=0;kb<BK;kb++){
        const __half2 scale2=__builtin_bit_cast(__half2,s_scale[kb*BM+row]);
        const uint32_t words[4]={raw[kb].x,raw[kb].y,raw[kb].z,raw[kb].w};
        __half2 h[16];
#pragma unroll
        for(int i=0;i<16;i++){
          const uint32_t b=(words[i>>2]>>(8*(i&3)))&0xffu;
          uint32_t bits;
          if constexpr(PermuteQ4){
            const uint32_t word=words[i>>2];
            const uint32_t selector=0x04040000u+0x01010101u*(i&3);
            const uint32_t paired=__builtin_amdgcn_perm(word>>4,word,selector);
            bits=(paired&0x000f000fu)|0x64006400u;
          }else bits=0x64006400u|(b&15u)|((b&240u)<<12);
          h[i]=__hsub2(__builtin_bit_cast(__half2,bits),__builtin_bit_cast(__half2,0x64086408u));
        }
        v16h a_lo,a_hi;
        __builtin_memcpy(&a_lo,&h[0],32);__builtin_memcpy(&a_hi,&h[8],32);
#pragma unroll
        for(int j=0;j<kTokTiles;j++){
          asm volatile("" ::: "memory");
          if(j>=live_tok_tiles)continue;
          const uint4* frag=s_act+(kb*4)*kActStride+j*16+sub_lane;
          uint4 b[4];
#pragma unroll
          for(int q=0;q<4;q++)b[q]=frag[q*kActStride];
          v16h b_lo,b_hi;
          __builtin_memcpy(&b_lo,&b[0],32);__builtin_memcpy(&b_hi,&b[2],32);
          group[j]=wmma(a_lo,b_lo,group[j]);
          group[j]=wmma(a_hi,b_hi,group[j]);
          const uint4* fl=s_act_lo+(kb*4)*kActStride+j*16+sub_lane;
          uint4 bl[4];
#pragma unroll
          for(int q=0;q<4;q++)bl[q]=fl[q*kActStride];
          v16h bl_lo,bl_hi;
          __builtin_memcpy(&bl_lo,&bl[0],32);__builtin_memcpy(&bl_hi,&bl[2],32);
          group_lo[j]=wmma(a_lo,bl_lo,group_lo[j]);
          group_lo[j]=wmma(a_hi,bl_hi,group_lo[j]);
          if((kb0+kb)%4==3){
            float sc=__low2float(scale2);
#pragma unroll
            for(int z=0;z<8;z++){
              float v=group[j][z];v+=group_lo[j][z]*(1.f/256);
              acc[j][z]=fmaf(v,__shfl(sc,2*z+half_id,32),acc[j][z]);
            }
            group[j]=v8f{};group_lo[j]=v8f{};
          }
        }
      }
  };
  fetch_stage(0);
  // Double buffering halves loop barriers without changing per-output math.
  // At K=2560: 41 barriers instead of 80; LDS 53,760 instead of 26,880 B.
  if constexpr(Double){
    commit_stage(0);moelut::stage_sync<LdsSync>();
    for(int kb0=0;kb0<num_kb;kb0+=BK){
      const int cur=(kb0/BK)&1;
      if(kb0+BK<num_kb)fetch_stage(kb0+BK);
      compute_stage(kb0,cur);
      if(kb0+BK<num_kb)commit_stage(cur^1);
      moelut::stage_sync<LdsSync>();
    }
  }else{
    for(int kb0=0;kb0<num_kb;kb0+=BK){
      commit_stage();moelut::stage_sync<LdsSync>();
      if(kb0+BK<num_kb)fetch_stage(kb0+BK);
      compute_stage(kb0);moelut::stage_sync<LdsSync>();
    }
  }

  // Reuse 16KiB of the now-dead staging LDS for one 16-token tile.
  // The 16 waves stage gate/up, then each wave owns one token / four values.
  // Both barriers are CTA-uniform, including partial token and expert tiles.
  float* base=reinterpret_cast<float*>(lds);
#pragma unroll
  for(int j=0;j<kTokTiles;j++){
    if(j>=live_tok_tiles)break;
#pragma unroll
    for(int z=0;z<8;z++)
      base[row_tile*2048+row_wave*256+sub_lane*16+2*z+half_id]=acc[j][z];
    __syncthreads();
    const int t16=wave_id,t=j*16+t16;
    float g[4],u[4],v[4];
#pragma unroll
    for(int q=0;q<4;q++){
      const int h=lane_id+q*32;
      const int idx=(h>>4)*256+t16*16+(h&15);
      g[q]=base[idx];u[q]=base[2048+idx];
    }
    r12_had128_wave4(g);r12_had128_wave4(u);
#pragma unroll
    for(int q=0;q<4;q++){
      const int c=r_block+lane_id+q*32;
      g[q]=g[q]*__half2float(gu_sv[c]);
      u[q]=u[q]*__half2float(gu_sv[c+mid]);
      v[q]=(g[q]/(1.f+expf(-g[q])))*u[q]*__half2float(dn_su[c]);
    }
    r12_had128_wave4(v);
    if(t<nrows){
      __half* dst=hid+(size_t)(slot0+t)*mid*2;
#pragma unroll
      for(int q=0;q<4;q++){
        const int c=r_block+lane_id+q*32;
        __half hi=__float2half(v[q]);dst[c]=hi;
        dst[mid+c]=__float2half((v[q]-__half2float(hi))*256.f);
      }
    }
    __syncthreads();
  }
}

}  // namespace r12guv3
