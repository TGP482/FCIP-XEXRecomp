// The renderer's scene: its pass, the pass's camera and the draw list the pass submit 82367D68 sorts and draws
#pragma once
#include <cstdint>

#include "xdk/hle.h"

namespace fcip::scene {

// renderer
constexpr uint32_t kRendererBlock = 0xC;  // the shader constant block, its draw list at +0x24
constexpr uint32_t kRendererPass = 0x90;  // the pass being submitted, its camera first
constexpr uint32_t kBlockList = 0x24;
// camera: row major, before the pass flips its depth
constexpr uint32_t kCameraView = 0x1BC;
constexpr uint32_t kCameraProjection = 0x23C;  // PerspectiveFov's right handed projection
// draw list 825091A0: the count first, entries {key, item, next} of 0xC bytes at +8, the bucket being popped
constexpr uint32_t kListEntries = 0x8;
constexpr uint32_t kListCursor = 0x201C;
constexpr uint32_t kListEntrySize = 0xC;
// keys: the layer in bits 16 to 19
constexpr uint32_t kLayerShift = 16;
constexpr uint32_t kLayerMask = 0xF;
constexpr uint32_t kLastWorldLayer = 4;

// the camera of the pass being submitted, 0 outside one
inline uint32_t Camera() {
    uint32_t renderer = kernel::R32(SYM_Renderer_pointer);
    uint32_t pass = renderer ? kernel::R32(renderer + kRendererPass) : 0;
    return pass ? kernel::R32(pass) : 0;
}

// the key of the item about to be popped, false when none is
inline bool NextKey(uint32_t list, uint32_t& key) {
    uint32_t cursor = kernel::R32(list + kListCursor);
    if (!kernel::R32(list) || !cursor) return false;
    int32_t index = int32_t(kernel::R32(cursor));
    if (index < 0) return false;
    key = kernel::R32(kernel::R32(list + kListEntries) + uint32_t(index) * kListEntrySize);
    return true;
}

}  // namespace fcip::scene
