#include "../src/gpu/ple_page_plan.h"
#include <iostream>
#include <random>

int main() {
  std::mt19937_64 rng(20261003);
  // Irregular EOF, two-page rows, duplicates and arbitrary completion order.
  for (int trial = 0; trial < 300; ++trial) {
    const size_t size = 8193 + rng() % 200000, n = 1 + rng() % 1024;
    const size_t width = trial % 7 == 0 ? 4096 : (trial % 2 ? 90 : 160);
    std::vector<uint8_t> file(size), expected(n * width), actual(n * width, 0);
    for (auto& b : file) b = (uint8_t)rng();
    std::vector<uint64_t> offsets(n);
    for (size_t i = 0; i < n; ++i) {
      offsets[i] = i && i % 3 == 0 ? offsets[i - 1] : rng() % (size - width + 1);
      std::memcpy(expected.data() + i * width, file.data() + offsets[i], width);
    }
    offsets.back() = size - width;
    std::memcpy(expected.data() + (n - 1) * width, file.data() + offsets.back(), width);
    auto p = ple_page::make(offsets.data(), n, width, file.size());
    for (size_t i = 1; i < p.pages.size(); ++i)
      if (p.pages[i - 1].file >= p.pages[i].file) return 1;
    std::shuffle(p.pages.begin(), p.pages.end(), rng);
    for (const auto& pg : p.pages) {
      if (pg.file + pg.bytes > file.size()) return 2;
      ple_page::scatter(p, pg, file.data() + pg.file, actual.data());
    }
    if (actual != expected) return 3;
  }
  uint64_t bad = 4090;
  try { ple_page::make(&bad, 1, 160, 4096); return 4; }
  catch (const std::out_of_range&) {}
  if (!ple_page::make(nullptr, 0, 160, 0).pages.empty()) return 5;
  std::cout << "300 randomized byte-equality plans + EOF/bounds/empty checks passed\n";
}
