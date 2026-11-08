// ige.xex: the instruction hooks of the shadow distance ([[hook]] in config/editor.toml)
#include "common/draw_distance.h"
#include "xdk/hle.h"

// engine+0x1174 as each reader loads it: the sun light radius f8, the range test f0
void ShadowRadiusLight(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[8]); }
void ShadowRadiusRange(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[0]); }
