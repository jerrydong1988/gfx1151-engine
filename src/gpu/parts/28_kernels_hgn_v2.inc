// Native v2 kernels. No requantization of the stored expert weights.
namespace hgnv2gpu {
__device__ float cb(uint16_t state) {
  uint32_t x = uint32_t(state)*0x83dcd12du;
  int s = (x&255)+((x>>8)&255)+((x>>16)&255)+(x>>24);
  return __half2float(__float2half_rn(fmaf(float(1024+s), __half2float(__ushort_as_half(0x1eee)),
                                        __half2float(__ushort_as_half(0xc931)))));
}
__device__ uint16_t bf(float v) {
  uint32_t u=__float_as_uint(v); return uint16_t((u+0x7fff+((u>>16)&1))>>16);
}
__global__ void q6_dense(const uint8_t* p, uint16_t* out, uint64_t n, int cols) {
  uint64_t i=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
  if(i>=n)return;
  int c=i%cols; const uint8_t* row=p+(i/cols)*((cols*13/16+15)&~15);
  int q=((row[c/2]>>((c%2)*4))&15)|(((row[cols/2+c/4]>>((c%4)*2))&3)<<4);
  const __half* sm=(const __half*)(row+cols*3/4+(c/64)*4);
  out[i]=bf(q*__half2float(sm[0])+__half2float(sm[1]));
}
// 128x128 output tile. Two normalized Walsh-Hadamard transforms in FP32.
__global__ void ht_dense(const uint32_t* packed, const __half* su, const __half* sv,
                         uint16_t* out, int cols) {
  __shared__ float tile[128*128];
  int r0=blockIdx.y*128,c0=blockIdx.x*128;
  for(int i=threadIdx.x;i<16384;i+=blockDim.x){
    int r=r0+i/128,c=c0+i%128;
    uint64_t ti=((uint64_t(r/128)*(cols/16)+c/16)*8+(r%128)/16)*32;
    int lane=r%16+(c%16/8)*16;
    uint64_t pair=(uint64_t(packed[ti+(lane+31)%32])<<32)|packed[ti+lane];
    tile[i]=cb(uint16_t(pair>>(28-4*(c%8))));
  }
  __syncthreads();
  for(int dim=0;dim<2;dim++) {
    for(int h=1;h<128;h*=2) {
      for(int p=threadIdx.x;p<8192;p+=blockDim.x){
        int a=(p/64)*128+(p%64/h)*2*h+p%h;
        if(dim) a=(a%128)*128+a/128;
        int b=a+h*(dim?128:1);float x=tile[a],y=tile[b];
        tile[a]=x+y;tile[b]=x-y;
      }
      __syncthreads();
    }
  }
  for(int i=threadIdx.x;i<16384;i+=blockDim.x){
    int r=r0+i/128,c=c0+i%128;
    out[uint64_t(r)*cols+c]=bf((tile[i]*(1.f/128))*__half2float(su[c])*__half2float(sv[r]));
  }
}
// Forward: H*diag(sign)*x. Inverse: diag(sign)*H*x. In-place safe per block.
__global__ void rotate(const float* x,float* y,const __half* sign,int cols,int rows,bool inverse){
  __shared__ float s[128];int j=threadIdx.x, col=blockIdx.x*128+j;
  uint64_t i=uint64_t(blockIdx.y)*cols+col;
  s[j]=x[i]*(inverse?1.f:__half2float(sign[col]));__syncthreads();
  for(int h=1;h<128;h*=2){float a=s[j],b=s[j^h];__syncthreads();s[j]=(j&h)?b-a:a+b;__syncthreads();}
  y[i]=s[j]*0.08838834764831844f*(inverse?__half2float(sign[col]):1.f);
}
// One wave per output row; slot selects an expert, input may be per token or slot.
__global__ void expert_mv(const uint8_t* q,const __half* scales,const float* x,float* y,
                          const int* ids,int cols,int erows,int topk,int slots,bool per_slot){
  int lane=threadIdx.x&31, row=blockIdx.x*(blockDim.x/32)+threadIdx.x/32;
  int slot=blockIdx.y;if(row>=erows||slot>=slots)return;
  int tok=slot/topk, rank=slot%topk, expert=ids[tok*16+rank];
  uint64_t off=(uint64_t(expert)*erows+row)*cols;
  const float* in=x+uint64_t(per_slot?slot:tok)*cols;float sum=0;
  for(int c=lane;c<cols;c+=32){uint64_t i=off+c;int v=((q[i/2]>>((i%2)*4))&15)-8;
    sum=fmaf(v*__half2float(scales[i/128]),in[c],sum);}
  for(int h=16;h;h/=2)sum+=__shfl_down(sum,h,32);
  if(lane==0)y[uint64_t(slot)*erows+row]=sum;
}
__global__ void reduce(const float* pairs,const float* weights,float* y,int cols,int topk){
  int c=blockIdx.x*blockDim.x+threadIdx.x,t=blockIdx.y;if(c>=cols)return;
  float sum=0;for(int k=0;k<topk;k++)sum=fmaf(pairs[(uint64_t(t)*topk+k)*cols+c],weights[t*16+k],sum);
  y[uint64_t(t)*cols+c]=sum;
}

// Five Hadamard stages stay in wave registers; only the two cross-wave
// stages touch LDS. The final barrier permits immediate reuse of scratch.
__device__ __forceinline__ float had128(float v,float* s,int j){
  for(int h=1;h<32;h*=2){float b=__shfl_xor(v,h,32);v=(j&h)?b-v:v+b;}
  s[j]=v;__syncthreads();
  float b=s[j^32];__syncthreads();v=(j&32)?b-v:v+b;
  s[j]=v;__syncthreads();
  b=s[j^64];__syncthreads();v=(j&64)?b-v:v+b;
  return v*0.08838834764831844f;
}
// Split stores hi and a scaled low residual in two FP16 planes per row.
// Packing once here avoids repeating conversion for every output-row tile.
template<class Out, bool Split=false>
__global__ void rotate_fast(const float* x,Out* y,const __half* sign,int cols,bool inverse){
  __shared__ float s[128];const int j=threadIdx.x,c=blockIdx.x*128+j;
  const uint64_t i=uint64_t(blockIdx.y)*cols+c;
  float v=x[i]*(inverse?1.f:__half2float(sign[c]));
  v=had128(v,s,j)*(inverse?__half2float(sign[c]):1.f);
  if constexpr(Split){
    __half* dst=(__half*)y+uint64_t(blockIdx.y)*cols*2;
    __half hi=__float2half(v);dst[c]=hi;dst[cols+c]=__float2half((v-__half2float(hi))*256.f);
  }else y[i]=Out(v);
}
// Gate/up accumulators are still in the rotated basis. Invert both, apply
// SiLU in the original basis, then rotate for down. The only F16 rounding
// for Out=half is the down GEMM input; transforms and activation remain FP32.
template<class Out, bool Split=false>
__global__ void activate_rotate(const float* gu,Out* hid,const __half* gu_sv,
                               const __half* dn_su,int mid){
  __shared__ float s[128];const int j=threadIdx.x,c=blockIdx.x*128+j;
  const uint64_t off=uint64_t(blockIdx.y)*2*mid+c;
  float g=had128(gu[off],s,j)*__half2float(gu_sv[c]);
  float u=had128(gu[off+mid],s,j)*__half2float(gu_sv[c+mid]);
  float v=(g/(1.f+expf(-g)))*u*__half2float(dn_su[c]);
  v=had128(v,s,j);
  if constexpr(Split){
    __half* dst=(__half*)hid+uint64_t(blockIdx.y)*mid*2;
    __half hi=__float2half(v);dst[c]=hi;dst[mid+c]=__float2half((v-__half2float(hi))*256.f);
  }else hid[uint64_t(blockIdx.y)*mid+c]=Out(v);
}

// Decode: one lane processes 32 adjacent weights, two rows share the input
// vector. Packed loads replace repeated byte/scale fetches of expert_mv.
// FP32 inputs and accumulation; no additional weight quantization.
__global__ void expert_mv_fast(const uint8_t* __restrict__ q,const __half* __restrict__ scales,
                               const float* __restrict__ x,float* __restrict__ y,
                               const int* __restrict__ ids,int cols,int erows,int topk,int slots,bool per_slot){
  const int lane=threadIdx.x&31;
  const int r=2*(blockIdx.x*(blockDim.x/32)+threadIdx.x/32),slot=blockIdx.y;
  if(r>=erows||slot>=slots)return;
  const int tok=slot/topk,expert=ids[tok*16+slot%topk];
  const uint64_t row=uint64_t(expert)*erows+r;
  const uint8_t* qa=q+row*(cols/2);const uint8_t* qb=qa+cols/2;
  const __half* sa=scales+row*(cols/128);const __half* sb=sa+cols/128;
  const float* in=x+uint64_t(per_slot?slot:tok)*cols;
  float sum0=0,sum1=0;
  for(int g=lane;g<cols/32;g+=32){
    const uint4 pa=*(const uint4*)(qa+g*16),pb=*(const uint4*)(qb+g*16);
    const uint32_t wa[4]={pa.x,pa.y,pa.z,pa.w},wb[4]={pb.x,pb.y,pb.z,pb.w};
    float a=0,b=0;
#pragma unroll
    for(int z=0;z<4;z++){
      uint32_t u=wa[z],v=wb[z];
#pragma unroll
      for(int j=0;j<8;j++){
        float xv=in[g*32+z*8+j];a=fmaf(float(int(u&15)-8),xv,a);b=fmaf(float(int(v&15)-8),xv,b);u>>=4;v>>=4;
      }
    }
    sum0=fmaf(a,__half2float(sa[g/4]),sum0);sum1=fmaf(b,__half2float(sb[g/4]),sum1);
  }
  for(int h=16;h;h/=2){sum0+=__shfl_down(sum0,h,32);sum1+=__shfl_down(sum1,h,32);}
  if(lane==0){y[uint64_t(slot)*erows+r]=sum0;y[uint64_t(slot)*erows+r+1]=sum1;}
}

// Native trellis GEMV: each wave owns 16 output rows; its two half waves
// cover the two eight-column halves of an HT tile. Splitting K fills the
// GPU for short matrices. Values are decoded to FP32 without a BF16 matrix.
__global__ void ht_mv(const uint32_t* __restrict__ q,const float* __restrict__ x,
                       float* __restrict__ parts,int rows,int cols,int split){
  const int lane=threadIdx.x&31,group=threadIdx.x/32;
  const int row=blockIdx.x*128+group*16+lane%16;
  const int slice=blockIdx.y,half=lane/16;
  float sum=0;
  for(int kb=slice;kb<cols/16;kb+=split){
    const uint64_t ti=((uint64_t(blockIdx.x)*(cols/16)+kb)*8+group)*32;
    uint32_t word=q[ti+lane],prev=__shfl(word,(lane+31)%32,32);
    const uint64_t pair=(uint64_t(prev)<<32)|word;
#pragma unroll
    for(int j=0;j<8;j++)sum=fmaf(cb(uint16_t(pair>>(28-4*j))),x[kb*16+half*8+j],sum);
  }
  sum+=__shfl_xor(sum,16,32);
  if(lane<16)parts[(size_t)slice*rows+row]=sum;
}
__global__ void ht_mv_finish(const float* parts,float* y,const __half* sv,int rows,int split){
  __shared__ float scratch[128];int j=threadIdx.x,r=blockIdx.x*128+j;float v=0;
  parts+=(size_t)blockIdx.y*split*rows;y+=(size_t)blockIdx.y*rows;
  for(int s=0;s<split;s++)v+=parts[(size_t)s*rows+r];
  y[r]=had128(v,scratch,j)*__half2float(sv[r]);
}
__global__ void ht_rotate_input(const void* x,float* y,const __half* su,int cols,
                                uint64_t stride,bool bf16){
  __shared__ float scratch[128];int j=threadIdx.x,c=blockIdx.x*128+j;
  uint64_t i=(uint64_t)blockIdx.y*stride+c;
  float v=bf16?__uint_as_float(uint32_t(((const uint16_t*)x)[i])<<16):((const float*)x)[i];
  y[(size_t)blockIdx.y*cols+c]=had128(v*__half2float(su[c]),scratch,j);
}
template<int BT>
__global__ void ht_mv_multi(const uint32_t* q,const float* x,float* parts,
                            int rows,int cols,int split,int P){
  int lane=threadIdx.x&31,group=threadIdx.x/32;
  int row=blockIdx.x*128+group*16+lane%16,slice=blockIdx.y,half=lane/16;
  int first=blockIdx.z*BT,live=min(BT,P-first);float acc[BT]{};
  for(int kb=slice;kb<cols/16;kb+=split){
    uint64_t ti=((uint64_t(blockIdx.x)*(cols/16)+kb)*8+group)*32;
    uint32_t word=q[ti+lane],prev=__shfl(word,(lane+31)%32,32);
    uint64_t pair=(uint64_t(prev)<<32)|word;
#pragma unroll
    for(int j=0;j<8;j++){
      float w=cb(uint16_t(pair>>(28-4*j)));
#pragma unroll
      for(int t=0;t<BT;t++)acc[t]=fmaf(w,x[(size_t)(first+min(t,live-1))*cols+kb*16+half*8+j],acc[t]);
    }
  }
#pragma unroll
  for(int t=0;t<BT;t++){
    acc[t]+=__shfl_xor(acc[t],16,32);
    if(lane<16 && t<live)parts[((size_t)(first+t)*split+slice)*rows+row]=acc[t];
  }
}

// FP32 batch fallback: each wave reuses one expert row for BT tokens while
// retaining expert_mv's column order and reduction tree exactly.
// Packed reads load four adjacent nibbles per lane, then redistribute them
// to c=128*g+32*j+lane. Multiplication, FMA and reduction order are unchanged;
// the 16-bit load is compressed integer storage, not an FP16 operand.
template<int BT, bool Packed=false>
__global__ void expert_batch_exact(const uint8_t* q,const __half* scales,
    const float* x,float* y,const int* tokidx,const int* tiles,const int* ntiles,
    int cols,int erows){
  if((int)blockIdx.y>=*ntiles)return;
  const int* tile=tiles+3*blockIdx.y;
  int start=BT*blockIdx.z;if(start>=tile[2])return;
  int lane=threadIdx.x&31,row=blockIdx.x*(blockDim.x/32)+threadIdx.x/32;
  if(row>=erows)return;
  const uint64_t off=(uint64_t(tile[0])*erows+row)*cols;
  int slot=tile[1]+start,live=min(BT,tile[2]-start);
  const float* in[BT];float acc[BT];
#pragma unroll
  for(int t=0;t<BT;t++){in[t]=x+(uint64_t)(tokidx?tokidx[slot+min(t,live-1)]:slot+min(t,live-1))*cols;acc[t]=0;}
  if constexpr(Packed){
    for(int g=0;g<cols/128;g++){
      uint32_t packed=((const uint16_t*)(q+off/2))[g*32+lane];
      float scale=__half2float(scales[off/128+g]);
#pragma unroll
      for(int j=0;j<4;j++){
        uint32_t bits=__shfl(packed,j*8+lane/4,32);
        float w=float(int((bits>>(4*(lane%4)))&15)-8)*scale;
        int c=g*128+j*32+lane;
#pragma unroll
        for(int t=0;t<BT;t++)acc[t]=fmaf(w,in[t][c],acc[t]);
      }
    }
  }else{
#pragma unroll 4
    for(int c=lane;c<cols;c+=32){
      uint64_t i=off+c;float w=float(int((q[i/2]>>((i%2)*4))&15)-8)*__half2float(scales[i/128]);
#pragma unroll
      for(int t=0;t<BT;t++)acc[t]=fmaf(w,in[t][c],acc[t]);
    }
  }
#pragma unroll
  for(int t=0;t<BT;t++){
    for(int h=16;h;h/=2)acc[t]+=__shfl_down(acc[t],h,32);
    if(lane==0 && t<live)y[uint64_t(slot+t)*erows+row]=acc[t];
  }
}
// Single/small-token counterpart with the same arithmetic as expert_mv.
__global__ void expert_mv_exact_packed(const uint8_t* q,const __half* scales,
    const float* x,float* y,const int* ids,int cols,int erows,int topk,int slots,bool per_slot){
  int lane=threadIdx.x&31,row=blockIdx.x*(blockDim.x/32)+threadIdx.x/32;
  int slot=blockIdx.y;if(row>=erows||slot>=slots)return;
  int tok=slot/topk,expert=ids[tok*16+slot%topk];
  uint64_t off=(uint64_t(expert)*erows+row)*cols;
  const float* in=x+uint64_t(per_slot?slot:tok)*cols;float sum=0;
  for(int g=0;g<cols/128;g++){
    uint32_t packed=((const uint16_t*)(q+off/2))[g*32+lane];
    float scale=__half2float(scales[off/128+g]);
#pragma unroll
    for(int j=0;j<4;j++){
      uint32_t bits=__shfl(packed,j*8+lane/4,32);
      float w=float(int((bits>>(4*(lane%4)))&15)-8)*scale;
      sum=fmaf(w,in[g*128+j*32+lane],sum);
    }
  }
  for(int h=16;h;h/=2)sum+=__shfl_down(sum,h,32);
  if(lane==0)y[(size_t)slot*erows+row]=sum;
}
__global__ void original_pairids(const int* sorted_values,int* pairids,int count){
  int p=blockIdx.x*blockDim.x+threadIdx.x;if(p<count)pairids[sorted_values[p]]=p;
}
} // namespace hgnv2gpu
