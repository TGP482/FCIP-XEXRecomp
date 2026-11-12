// the pass the renderer's pass submit (vtable +0x2C, 82367D68) is drawing
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip {

constexpr uint32_t kPassCamera = 5;  // the camera's view; 2 is the sun's shadow map

// the type of the pass being submitted on this thread, 0 outside one
uint32_t SubmittedPass();

// the sun shadow map's guest side, 1344 at stock
uint32_t SunShadowSide();

// ShadowMaps_Update: past 1x shadow distance sharper sun maps first, then the game's own
void DrawShadowMaps(PPCContext& ctx, uint8_t* base);

constexpr uint32_t kSunMaps = 5;        // sharper maps at most, map i at 2^i times the stock radius
constexpr uint32_t kSunFetch = 8;       // SunShadowSampler in every receiver
constexpr uint32_t kSunFirstSlot = 27;  // the pixel shader views the sharper maps are bound to, through 31

// the sun maps as the renderer reads them: receivers fetch the far one with kSunFetch, distance is its radius over the
// stock one
struct SunMaps {
    uint32_t far_base = 0;
    uint32_t bases[kSunMaps] = {};
    uint32_t count = 0;
    uint32_t distance = 0;
};
SunMaps SunMapsNow();
constexpr uint8_t kSunCascade = 1;     // ShaderOptions::tex_flags on a fetch of the far map

}  // namespace fcip
