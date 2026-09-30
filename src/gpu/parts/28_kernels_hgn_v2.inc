// Correctness-first v2 kernels. No requantization of v2 expert weights.
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
} // namespace hgnv2gpu
