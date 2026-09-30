// Independent reference decoding for the Flash-Next v2 HGN layouts.
// See docs/HGN_V2.md for byte layouts, provenance and validation limits.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hgn_v2 {
inline uint16_t u16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline float half(uint16_t x) {
  const int e = (x >> 10) & 31, m = x & 1023;
  float f = e == 31 ? (m ? NAN : INFINITY) :
            e == 0 ? std::ldexp((float)m, -24) : std::ldexp((float)(1024 + m), e - 25);
  return x & 32768 ? -f : f;
}
// Round a finite float to half, ties to even. Used by the procedural codebook.
inline float half_round(float f) {
  if (f == 0) return f;
  int e; std::frexp(std::fabs(f), &e);
  float step = std::ldexp(1.0f, e < -13 ? -24 : e - 11);
  float a = std::fabs(f) / step, lo = std::floor(a), frac = a - lo;
  if (frac > .5f || (frac == .5f && std::fmod(lo, 2.0f) != 0)) lo++;
  return std::copysign(lo * step, f);
}
inline float trellis_value(uint16_t state) {
  const uint32_t x = uint32_t(state) * 0x83dcd12du;
  const int sum = (x & 255) + ((x >> 8) & 255) + ((x >> 16) & 255) + (x >> 24);
  return half_round(std::fma(float(1024 + sum), half(0x1eee), half(0xc931)));
}
inline void had128(float* x, size_t stride = 1) {
  for (int h = 1; h < 128; h *= 2)
    for (int b = 0; b < 128; b += 2 * h)
      for (int j = 0; j < h; j++) {
        float a = x[(b+j)*stride], c = x[(b+j+h)*stride];
        x[(b+j)*stride] = a+c; x[(b+j+h)*stride] = a-c;
      }
  for (int j = 0; j < 128; j++) x[j*stride] *= 0.08838834764831844f;
}
inline void validate(uint32_t dtype, uint32_t param, size_t rows, size_t cols, size_t bytes) {
  if (!rows || !cols) throw std::runtime_error("HGN v2: empty matrix");
  bool ok = false;
  if (dtype == 16) ok = param == 0x1208 && rows % 128 == 0 && cols % 128 == 0 && bytes == rows*cols/2;
  if (dtype == 23) ok = param == 128 && rows % 128 == 0 && cols % 128 == 0 && bytes == rows*cols/2 + rows*cols/64;
  if (dtype == 24) ok = param == 64 && cols % 64 == 0 && bytes == rows*((cols*13/16+15)&~size_t(15));
  if (!ok) throw std::runtime_error("HGN v2: unsupported variant, shape or payload size");
}
inline float q6_at(const uint8_t* p, size_t cols, size_t row, size_t col) {
  p += row*((cols*13/16+15)&~size_t(15));
  int code = ((p[col/2] >> ((col%2)*4)) & 15) |
             (((p[cols/2+col/4] >> ((col%4)*2)) & 3) << 4);
  const uint8_t* sm = p+cols*3/4+(col/64)*4;
  return code*half(u16(sm))+half(u16(sm+2));
}
inline float ht_at(const uint8_t* p, size_t cols, size_t row, size_t col) {
  size_t tile = ((row/128)*(cols/16)+col/16)*8+(row%128)/16;
  size_t lane = row%16 + ((col%16)/8)*16;
  const uint8_t* t = p + tile*128;
  uint64_t pair = (uint64_t(u32(t+((lane+31)%32)*4))<<32) | u32(t+lane*4);
  return trellis_value(uint16_t(pair >> (28-4*(col%8))));
}
// A row range must contain complete Hadamard blocks; experts share the signs.
inline void dequant(uint32_t dtype, uint32_t param, const uint8_t* p,
                    size_t rows, size_t cols, size_t bytes, const uint8_t* su,
                    const uint8_t* sv, size_t expert_rows, size_t first, size_t count,
                    float* out) {
  validate(dtype,param,rows,cols,bytes);
  if (first > rows || count > rows-first) throw std::runtime_error("HGN v2: row range");
  if (dtype == 24) {
    for (size_t r=0;r<count;r++) for(size_t c=0;c<cols;c++) out[r*cols+c]=q6_at(p,cols,first+r,c);
    return;
  }
  if (!su || !sv || !expert_rows || expert_rows%128 || first%128 || count%128)
    throw std::runtime_error("HGN v2: missing rotation or unaligned row range");
  for (size_t r=0;r<count;r++) for(size_t c=0;c<cols;c++) {
    size_t i=(first+r)*cols+c;
    out[r*cols+c] = dtype == 16 ? ht_at(p,cols,first+r,c) :
      (int((p[i/2] >> ((i%2)*4))&15)-8)*half(u16(p+rows*cols/2+(i/128)*2));
  }
  for(size_t r=0;r<count;r++) for(size_t c=0;c<cols;c+=128) had128(out+r*cols+c);
  for(size_t r=0;r<count;r+=128) for(size_t c=0;c<cols;c++) had128(out+r*cols+c,cols);
  for(size_t r=0;r<count;r++) for(size_t c=0;c<cols;c++)
    out[r*cols+c] *= half(u16(su+c*2))*half(u16(sv+((first+r)%expert_rows)*2));
}
} // namespace hgn_v2
