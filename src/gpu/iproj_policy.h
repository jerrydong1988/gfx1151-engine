#pragma once
#include <climits>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>

namespace iproj_policy {
// An event API success does not guarantee a usable duration on every runtime.
// Empty profiling scopes may take zero time; a timed GEMM must be positive.
inline bool valid_duration(float ms, bool allow_zero = false) {
  return std::isfinite(ms) && (allow_zero ? ms >= 0.f : ms > 0.f);
}
// INT_MIN means the unchanged automatic policy. INT_MAX is the engine's
// existing plain-SGEMM sentinel. Runtime solution IDs are not portable.
inline bool parse(const char* text, int& solution) {
  solution = INT_MIN;
  if (!text || !*text || !std::strcmp(text, "auto")) return true;
  const bool negative = *text == '-';
  if (negative) ++text;
  if (!*text) return false;
  uint32_t magnitude = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const uint32_t digit = (uint32_t)(*text - '0');
    if (magnitude > ((uint32_t)INT_MAX - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  if (!negative && magnitude == INT_MAX) return false;
  solution = negative ? -(int)magnitude : (int)magnitude;
  return true;
}
} // namespace iproj_policy
