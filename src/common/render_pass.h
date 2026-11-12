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

// in ShadowMaps_Update before its own run: the near sun map past 1x shadow distance
void DrawNearShadow(PPCContext& ctx, uint8_t* base);

}  // namespace fcip
