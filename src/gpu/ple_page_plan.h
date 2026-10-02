#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

// Pure byte I/O planning. Hashing, FP8 decoding and GPU row order are unchanged.
namespace ple_page {
constexpr uint32_t page_bytes = 4096;
struct Fragment { uint64_t file; size_t output; uint32_t bytes; };
struct Page { uint64_t file; uint32_t bytes; size_t begin, end; };
struct Plan { std::vector<Fragment> fragments; std::vector<Page> pages; };

inline Plan make(const uint64_t* offsets, size_t n, size_t row_bytes,
                 uint64_t file_bytes) {
  if (!row_bytes || row_bytes > page_bytes ||
      n > std::numeric_limits<size_t>::max() / row_bytes)
    throw std::invalid_argument("invalid PLE page-plan extent");
  Plan plan;
  for (size_t i = 0; i < n; ++i) {
    if (offsets[i] > file_bytes || row_bytes > file_bytes - offsets[i])
      throw std::out_of_range("PLE row outside file");
    size_t left = row_bytes, copied = 0;
    while (left) {
      const uint64_t file = offsets[i] + copied;
      const uint32_t bytes = (uint32_t)std::min<size_t>(left, page_bytes - file % page_bytes);
      plan.fragments.push_back({file, i * row_bytes + copied, bytes});
      copied += bytes; left -= bytes;
    }
  }
  std::sort(plan.fragments.begin(), plan.fragments.end(),
            [](const Fragment& a, const Fragment& b) { return a.file < b.file; });
  for (size_t begin = 0; begin < plan.fragments.size();) {
    const uint64_t base = plan.fragments[begin].file / page_bytes * page_bytes;
    size_t end = begin + 1;
    while (end < plan.fragments.size() && plan.fragments[end].file / page_bytes * page_bytes == base) ++end;
    plan.pages.push_back({base, (uint32_t)std::min<uint64_t>(page_bytes, file_bytes - base), begin, end});
    begin = end;
  }
  return plan;
}

inline void scatter(const Plan& plan, const Page& page, const uint8_t* bytes,
                    uint8_t* output) {
  for (size_t i = page.begin; i < page.end; ++i) {
    const auto& f = plan.fragments[i];
    std::memcpy(output + f.output, bytes + (size_t)(f.file - page.file), f.bytes);
  }
}
}  // namespace ple_page
