#pragma once
#include <bit>
#include <cstdint>

#include "gpu/state.h"

// Ambient occlusion, global illumination and volumetric fog reach the renderer as a clear with a flag no D3DCLEAR
// value uses, so the render thread keeps it in order with the draws; what it needs rides in the clear's colour,
// depth, stencil and rect
namespace gpu {

constexpr uint32_t kClearOcclusion = 0x100;
constexpr uint32_t kClearOcclusionScene = 0x200;  // RTAO traces the world drawn from here to the occlusion
// global illumination: the world's draws stream into its scene and write their albedo, the draws past the world clear
// theirs and the world's end brings the game's ambient in its colour, then the scene's light is traced and added after
// its last draw
constexpr uint32_t kClearIlluminationScene = 0x10000;
constexpr uint32_t kClearIlluminationWorld = 0x20000;
constexpr uint32_t kClearIllumination = 0x40000;
// volumetric fog after the camera pass's last draw, its method the FogViewAt slot
constexpr uint32_t kClearFog = 0x80000;

// the ambient occlusion setting
enum : uint32_t { kAoSsao = 1, kAoGtao = 2, kAoRtao = 3 };
// inline ray tracing for RTAO, known once the renderer made its device
bool RayTracing();

struct OcclusionArgs {
    float proj[4] = {};  // 1 / x scale, 1 / y scale, x and y centre: view x = (ndc x - centre) / scale * distance
    float m22 = 0;       // view distance = m32 / (ndc depth + m22), a right handed projection
    float m32 = 0;
    float fog_scale = 0;  // the fogged share = saturate(scale * view distance + bias)
    float fog_bias = 0;
    uint32_t fog_colour = 0;  // 0xRRGGBB
    uint32_t method = 0;      // kAo*
};

// volumetric fog, from the renderer's view space (x right, y up, z the distance)
struct FogView {
    float sun[16] = {};     // into the far sun map's clip space, column j from sun[j * 4]
    float height[4] = {};   // to the height over the sea
    float sun_colour[4] = {};
    bool shadowed = false;  // without a sun map the sun lights none of it
};
// render thread: the view a kClearFog marker names
FogView FogViewAt(uint32_t slot);

inline void PackOcclusion(const OcclusionArgs& a, float color[4], float& z, uint32_t& stencil, Rect& r) {
    for (int i = 0; i < 4; i++) color[i] = a.proj[i];
    z = a.m32;
    stencil = std::bit_cast<uint32_t>(a.m22);
    r = {std::bit_cast<int32_t>(a.fog_scale), std::bit_cast<int32_t>(a.fog_bias), int32_t(a.fog_colour),
         int32_t(a.method)};
}

inline OcclusionArgs UnpackOcclusion(const float color[4], float z, uint32_t stencil, const Rect& r) {
    OcclusionArgs a;
    for (int i = 0; i < 4; i++) a.proj[i] = color[i];
    a.m32 = z;
    a.m22 = std::bit_cast<float>(stencil);
    a.fog_scale = std::bit_cast<float>(r.x1);
    a.fog_bias = std::bit_cast<float>(r.y1);
    a.fog_colour = uint32_t(r.x2);
    a.method = uint32_t(r.y2);
    return a;
}

}  // namespace gpu

namespace d3d {
// on the title's thread between the draws it goes after, over dev's bound colour and depth targets
void Occlusion(uint32_t dev, const gpu::OcclusionArgs& a);
// on the title's thread before the world's draws RTAO traces
void OcclusionScene(uint32_t dev);
// on the title's thread before the camera pass's list, after its world and after its last item
void IlluminationScene(uint32_t dev);
void IlluminationWorld(uint32_t dev, const float ambient[3]);
void Illumination(uint32_t dev, const gpu::OcclusionArgs& a);
// on the title's thread after the camera pass's last item
void VolumetricFog(uint32_t dev, const gpu::OcclusionArgs& a, const gpu::FogView& v);
}  // namespace d3d
