#pragma once

#include <cstdint>

// Shared by the HTTP frontend and the engine's VIMG receiver. These are
// admission limits, not preallocations or changes to image resolution.
namespace gdec_vision_limits {
constexpr int kMaxImages = 32;
// 1.5 GiB of FP32 patches (1536 floats per patch). This admits 32 1080p
// images and every previously accepted 16-image API request at its existing
// per-image resize ceiling, without doubling the worst-case patch storage.
constexpr uint64_t kMaxPatches = 262144;

inline bool admit_patches(uint64_t count, uint64_t* used) {
    if (*used > kMaxPatches || count > kMaxPatches - *used) return false;
    *used += count;
    return true;
}
}  // namespace gdec_vision_limits
