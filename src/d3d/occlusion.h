#pragma once
#include <bit>
#include <cstdint>
#include <vector>

#include "gpu/state.h"

// Ambient occlusion, global illumination, volumetric fog and clouds reach the renderer as a clear with a flag no
// D3DCLEAR value uses, so the render thread keeps it in order with the draws; what it needs rides in the clear's
// colour, depth, stencil and rect
namespace gpu {

constexpr uint32_t kClearOcclusion = 0x100;
constexpr uint32_t kClearOcclusionScene = 0x200;  // RTAO traces the world drawn from here to the occlusion
// global illumination: the world's draws stream into its scene and write their albedo, the draws past the world write
// theirs too, the first person weapon among them, but the ocean's clear it; the world's end brings the game's ambient
// in its colour, then the scene's light is traced and added after its last draw, its method the FogViewAt slot of the
// sun that lights hidden hits
constexpr uint32_t kClearIlluminationScene = 0x10000;
constexpr uint32_t kClearIlluminationWorld = 0x20000;
constexpr uint32_t kClearIllumination = 0x40000;
constexpr uint32_t kClearIlluminationOcean = 0x100000;  // the ocean's draws, until kClearIlluminationPastOcean
constexpr uint32_t kClearIlluminationPastOcean = 0x200000;
// volumetric fog over the camera pass's draws so far, its method the FogViewAt slot; the draws after it take it per
// pixel until kClearFogEnd
constexpr uint32_t kClearFog = 0x80000;
constexpr uint32_t kClearFogEnd = 0x400000;
// volumetric clouds over the sky box drawn so far, its view the CloudViewAt slot
constexpr uint32_t kClearClouds = 0x800000;

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

// volumetric fog and global illumination's sun, from the renderer's view space (x right, y up, z the distance)
struct FogView {
    float sun[16] = {};     // into the far sun map's clip space, column j from sun[j * 4]
    float height[4] = {};   // to the height over the sea
    float sun_colour[4] = {};
    float terrain[8] = {};  // into the terrain shade's texels, x then y
    bool shadowed = false;  // without a sun map the sun lights none of it
    bool shaded = false;    // with a terrain shade
};
// render thread: the view a kClearFog or kClearIllumination marker names
FogView FogViewAt(uint32_t slot);

// volumetric clouds, from the renderer's view space; the world's z is up
struct CloudView {
    float to_world[12] = {};  // rows taking a view direction into the world, three floats each and one unused
    float sun[4] = {};        // toward the sun in the world
    float sun_colour[4] = {};
    float sky[4] = {};        // the sky box's turn as its cos and sin, the camera's height over the sea, wind seconds
    float weather[4] = {};    // coverage from 0 to 1, rain from 0 to 1
};
// render thread: the view a kClearClouds marker names
CloudView CloudViewAt(uint32_t slot);

// the volumetric fog's terrain shade: a square of heights over the sea below which the terrain hides the sun
struct TerrainShade {
    uint32_t side = 0;
    std::vector<float> heights;
};
// render thread: the shade published since generation seen, its generation, else seen
uint32_t TerrainShadeSince(uint32_t seen, TerrainShade& out);

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
// past the world, on the ocean's first draw and the first after it
void IlluminationOcean(uint32_t dev, bool ocean);
void Illumination(uint32_t dev, const gpu::OcclusionArgs& a, const gpu::FogView& v);
// on the title's thread before the camera pass's first item on the camera's side of the water, else after its last
void VolumetricFog(uint32_t dev, const gpu::OcclusionArgs& a, const gpu::FogView& v);
// after the camera pass's last item
void VolumetricFogEnd(uint32_t dev);
// on the title's thread after the sky box's draw
void VolumetricClouds(uint32_t dev, const gpu::OcclusionArgs& a, const gpu::CloudView& v);
// title thread: a new terrain shade for the volumetric fog
void PublishTerrainShade(gpu::TerrainShade&& shade);
}  // namespace d3d
