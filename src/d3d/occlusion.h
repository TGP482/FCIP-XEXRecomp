#pragma once
#include <bit>
#include <cstdint>

#include "gpu/state.h"

// Ambient occlusion reaches the renderer as a clear with a flag no D3DCLEAR value uses, so the render thread keeps it
// in order with the draws; what it needs rides in the clear's colour, depth, stencil and rect
namespace gpu {

constexpr uint32_t kClearOcclusion = 0x100;

struct OcclusionArgs {
    float proj[4] = {};  // 1 / x scale, 1 / y scale, x and y centre: view x = (ndc x - centre) / scale * distance
    float m22 = 0;       // view distance = m32 / (ndc depth + m22), a right handed projection
    float m32 = 0;
    float fog_scale = 0;  // the fogged share = saturate(scale * view distance + bias)
    float fog_bias = 0;
    uint32_t fog_colour = 0;  // 0xRRGGBB
    uint32_t gtao = 0;        // 1 GTAO, 0 SSAO
};

inline void PackOcclusion(const OcclusionArgs& a, float color[4], float& z, uint32_t& stencil, Rect& r) {
    for (int i = 0; i < 4; i++) color[i] = a.proj[i];
    z = a.m32;
    stencil = std::bit_cast<uint32_t>(a.m22);
    r = {std::bit_cast<int32_t>(a.fog_scale), std::bit_cast<int32_t>(a.fog_bias), int32_t(a.fog_colour),
         int32_t(a.gtao)};
}

inline OcclusionArgs UnpackOcclusion(const float color[4], float z, uint32_t stencil, const Rect& r) {
    OcclusionArgs a;
    for (int i = 0; i < 4; i++) a.proj[i] = color[i];
    a.m32 = z;
    a.m22 = std::bit_cast<float>(stencil);
    a.fog_scale = std::bit_cast<float>(r.x1);
    a.fog_bias = std::bit_cast<float>(r.y1);
    a.fog_colour = uint32_t(r.x2);
    a.gtao = uint32_t(r.y2);
    return a;
}

}  // namespace gpu

namespace d3d {
// on the title's thread between the draws it goes after, over dev's bound colour and depth targets
void Occlusion(uint32_t dev, const gpu::OcclusionArgs& a);
}  // namespace d3d
