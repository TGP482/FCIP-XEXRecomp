// the pass the renderer's pass submit (vtable +0x2C, 82367D68) is drawing
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct PPCContext;

namespace fcip {

constexpr uint32_t kPassCamera = 5;  // the camera's view
constexpr uint32_t kPassShadow = 2;  // a light's shadow map, the sun's among them

// the type of the pass being submitted on this thread, 0 outside one
uint32_t SubmittedPass();
// title thread: the shadow update's run for the volumetric fog, which every object casts into
bool FogMapRun();

// the sun shadow map's guest side, 1344 at stock
uint32_t SunShadowSide();

// ShadowMaps_Update: past 1x shadow distance sharper sun maps first, then the game's own
void DrawShadowMaps(PPCContext& ctx, uint8_t* base);

// sharper maps at most, map i at 2^i times the stock radius; a 16x map would redraw nearly the far map's casters, as
// object draw distance already cuts most past it
constexpr uint32_t kSunMaps = 4;
constexpr uint32_t kSunFetch = 8;       // SunShadowSampler in every receiver
constexpr uint32_t kSunFirstSlot = 27;  // the pixel shader views the sharper maps are bound to, through 30

// the sun maps as the renderer reads them: receivers fetch the far one with kSunFetch, distance is its radius over the
// stock one
struct SunMaps {
    uint32_t far_base = 0;
    uint32_t fog_base = 0;  // the far map with every object casting, 0 when none was drawn for the fog
    uint32_t bases[kSunMaps] = {};
    uint32_t count = 0;
    uint32_t distance = 0;
    uint32_t side = 0;       // the far map's guest side, half inside a vis area
    uint32_t near_side = 0;  // the sharper maps' guest side
    float depth_ratio = 0;   // the far map's depth range over its width
};
constexpr int kSunFar = int(kSunMaps);  // SunResolved's answer for the far map

// render thread: each resolve's address, answering which sun map it is, -1 for none; then the maps as of the last far
// map resolve
int SunResolved(uint32_t address);
SunMaps SunMapsDrawn();
// title thread: world space into the far sun map as its last pass drew it, row major with the pass's depth flip, so
// nearer the sun is deeper; false before one was drawn
bool SunClip(float out[16]);
constexpr uint8_t kSunCascade = 1;     // ShaderOptions::tex_flags on a fetch of the far map
constexpr uint8_t kSunSoft = 2;        // the same, with the soft filter on
constexpr uint8_t kShadowSoft = 4;     // another light's shadow map, soft
constexpr uint8_t kShadowHalf = 8;     // that map drawn as one of four inside a vis area, 672 at stock
constexpr uint8_t kSunPcss = 16;       // the far sun map with PCSS on
constexpr uint8_t kShadowRay = 32;     // any shadow map, traced against its casters; the scene's slot in the top bits
constexpr uint32_t kRaySlotShift = 6;
constexpr uint32_t kRayScenes = 4;      // shadow maps one draw traces at most
constexpr uint32_t kRayFirstSlot = 25;  // pixel shader views: casters, their records, then the scenes through 30
// global illumination: a colour texture, never a shadow map, whose first fetch is the albedo; set on kAlbedoOut it
// writes the albedo target, which no draw reaches as a texture of its own
constexpr uint8_t kAlbedoFetch = 64;
constexpr uint32_t kAlbedoOut = 31;
constexpr uint32_t kAlbedoTarget = 5;  // past the motion target
// GlobalVariables.fx registers: the colour the game's materials lay over their texture, and the ambient lighting them
constexpr uint32_t kMaterialColour = 5;  // PixelGlobal.MaterialDiffuse
constexpr uint32_t kPsAmbient = 4;       // PixelGlobal.AmbientColor
constexpr uint32_t kVsAmbient = 128;     // VertexGlobal.AmbientColor
// the translated shader reads constant reg
bool ReadsConstant(const std::string& hlsl, uint32_t reg);

// original Xbox ocean: on its reflection fetch and kOceanOut; g_ext.x ReflectAmount, g_ext.y GlowBoostLevel
constexpr uint8_t kOceanReflection = 128;
constexpr uint32_t kOceanOut = 30;
struct OceanLook {
    float amount = 0;
    float pivot = 0;
};
// title thread, each camera pass
void SetOceanLook(bool on, const OceanLook& look);
// render thread: false when off
bool OceanLookDrawn(OceanLook& out);

// HLSL of CasterHit(caster, prim, bary): t_ray_casters and t_ray_records declared before it
extern const char* const kCasterHitHlsl;

// what a pixel shader's first instruction fetches its first texture at: an interpolator and two of its components,
// reg -1 when that instruction is no such fetch
struct FirstFetch {
    int8_t reg = -1;
    uint8_t x = 0;
    uint8_t y = 1;
    bool colour = false;  // its blue is read: no normal map, which the game reads as wyx
};
FirstFetch FindFirstFetch(const uint32_t* code, size_t dwords);

}  // namespace fcip
