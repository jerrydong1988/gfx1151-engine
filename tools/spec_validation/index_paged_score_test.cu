// Synthetic raw-bit regression for optional direct-paged score64.
// Build from the repository root (the optional kernel is included in this branch):
// hipcc -O3 -std=c++17 --offload-arch=gfx1151 -Isrc/gpu tools/spec_validation/index_paged_score_test.cu -o build/index_paged_score_test
// This executable uses the GPU only when the operator explicitly runs it.
// It does not load a model, start services, or measure performance.
#include <hip/hip_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#include "parts/09_kernels_index.inc"

#define CK(x) do { hipError_t e = (x); if (e != hipSuccess) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, hipGetErrorString(e)); \
  std::exit(2); } } while (0)

__global__ void reference_gather(const float* source, const int* table,
                                 float* dest, int nb) {
  const int b = (int)blockIdx.x;
  if (b < nb) dest[(size_t)b * 128 + threadIdx.x] =
      source[(size_t)ik_blk(table, b) * 128 + threadIdx.x];
}

struct Case { int nb, first, count; };

static bool check_case(const Case& c, int layout) {
  std::mt19937 rng(9017u + (unsigned)c.nb * 3u + (unsigned)layout);
  const int logical_pages = (c.nb + KV_PAGE_BLOCKS - 1) / KV_PAGE_BLOCKS;
  const int physical_pages = logical_pages + 7;  // holes + unused poison pages
  std::vector<int> table(logical_pages), pool(physical_pages);
  std::iota(pool.begin(), pool.end(), 0);
  if (layout == 1) std::reverse(pool.begin(), pool.end());
  if (layout == 2) std::shuffle(pool.begin(), pool.end(), rng);
  std::copy_n(pool.begin(), logical_pages, table.begin());
  std::vector<float> queries((size_t)c.count * 512), logical((size_t)c.nb * 128);
  for (auto& x : queries) x = ((int)(rng() % 2049) - 1024) / 256.0f;
  for (auto& x : logical) x = ((int)(rng() % 2049) - 1024) / 128.0f;
  const uint32_t poison_bits = 0x7fc12345u;
  float poison; std::memcpy(&poison, &poison_bits, sizeof(poison));
  std::vector<float> physical((size_t)physical_pages * KV_PAGE_BLOCKS * 128, poison);
  for (int b = 0; b < c.nb; ++b) {
    // CPU reference uses quotient/remainder, independently of device ik_blk.
    const int phys = table[b / KV_PAGE_BLOCKS] * KV_PAGE_BLOCKS + b % KV_PAGE_BLOCKS;
    std::copy_n(logical.data() + (size_t)b * 128, 128,
                physical.data() + (size_t)phys * 128);
  }
  float *dq, *dp, *dg, *ds0, *ds1; int* dt;
  const size_t n = (size_t)c.count * c.nb, padded = n + 16;
  CK(hipMalloc(&dq, queries.size() * sizeof(float)));
  CK(hipMalloc(&dp, physical.size() * sizeof(float)));
  CK(hipMalloc(&dg, logical.size() * sizeof(float)));
  CK(hipMalloc(&dt, table.size() * sizeof(int)));
  CK(hipMalloc(&ds0, padded * sizeof(float)));
  CK(hipMalloc(&ds1, padded * sizeof(float)));
  CK(hipMemcpy(dq, queries.data(), queries.size() * sizeof(float), hipMemcpyHostToDevice));
  CK(hipMemcpy(dp, physical.data(), physical.size() * sizeof(float), hipMemcpyHostToDevice));
  CK(hipMemcpy(dt, table.data(), table.size() * sizeof(int), hipMemcpyHostToDevice));
  reference_gather<<<c.nb, 128>>>(dp, dt, dg, c.nb);
  CK(hipGetLastError());
  std::vector<float> gathered(logical.size());
  CK(hipMemcpy(gathered.data(), dg, gathered.size() * sizeof(float), hipMemcpyDeviceToHost));
  const bool gathered_ok = std::memcmp(gathered.data(), logical.data(), logical.size() * sizeof(float)) == 0;
  std::vector<uint32_t> h0(padded, poison_bits), h1(padded, poison_bits);
  CK(hipMemcpy(ds0, h0.data(), padded * sizeof(float), hipMemcpyHostToDevice));
  CK(hipMemcpy(ds1, h1.data(), padded * sizeof(float), hipMemcpyHostToDevice));
  k_index_scores_t64<false><<<index_t64_grid(c.nb, c.count), 256>>>(
      dq, dg, ds0, c.nb, c.count, c.first);
  CK(hipGetLastError());
  k_index_scores_t64<true><<<index_t64_grid(c.nb, c.count), 256>>>(
      dq, dp, ds1, c.nb, c.count, c.first, dt);
  CK(hipGetLastError());
  CK(hipDeviceSynchronize());
  CK(hipMemcpy(h0.data(), ds0, padded * sizeof(float), hipMemcpyDeviceToHost));
  CK(hipMemcpy(h1.data(), ds1, padded * sizeof(float), hipMemcpyDeviceToHost));
  size_t bit_different_elements = 0, bad_write_mask = 0, valid_scores = 0;
  for (size_t i = 0; i < padded; ++i) {
    bit_different_elements += h0[i] != h1[i];
    const int row = (int)(i / c.nb), b = (int)(i % c.nb);
    const bool valid = i < n && b < std::min(c.nb, (c.first + row + 1) / 4);
    if (valid) {
      ++valid_scores;
      float v0, v1;
      std::memcpy(&v0, &h0[i], sizeof(float));
      std::memcpy(&v1, &h1[i], sizeof(float));
      bad_write_mask += !std::isfinite(v0) || !std::isfinite(v1);
    } else {
      bad_write_mask += h0[i] != poison_bits || h1[i] != poison_bits;
    }
  }
  const bool pass = gathered_ok && valid_scores > 0 && bit_different_elements == 0 && bad_write_mask == 0;
  std::printf("{\"nb\":%d,\"first\":%d,\"count\":%d,\"layout\":%d,\"gather_cpu_bits_equal\":%s,\"valid_scores\":%zu,\"different_score_elements\":%zu,\"bad_write_mask\":%zu,\"pass\":%s}\n",
      c.nb, c.first, c.count, layout, gathered_ok ? "true" : "false", valid_scores,
      bit_different_elements, bad_write_mask, pass ? "true" : "false");
  CK(hipFree(dq)); CK(hipFree(dp)); CK(hipFree(dg)); CK(hipFree(dt)); CK(hipFree(ds0)); CK(hipFree(ds1));
  return pass;
}

int main() {
  const Case cases[] = {
    {65, 240, 65}, {257, 991, 65}, {513, 2051, 16}, {576, 2243, 65},
    {8193, 32739, 67}, {32768, 130989, 83}, {65000, 259936, 64}, {65536, 262073, 65}
  };
  int passed = 0, total = 0;
  for (const auto& c : cases) for (int layout = 0; layout < 3; ++layout) {
    passed += check_case(c, layout); ++total;
  }
  std::printf("{\"summary\":true,\"passed\":%d,\"total\":%d}\n", passed, total);
  return passed == total ? 0 : 1;
}
