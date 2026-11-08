// ige.xex: the instruction hooks of the draw distance and the grass cache ([[hook]] in config/editor.toml); the editor
// loads every sector at level load, so it needs no sector streaming
#include "common/draw_distance.h"
#include "common/grass_tiles.h"
#include "xdk/hle.h"

// f28 = engine+0xB30 from its getter, squared for the tile loop's radius and stored to params+0x1C
void GrassRadius(PPCContext& ctx, uint8_t*) { fcip::ScaleGrassRadius(ctx.f[28]); }

// engine+0x1174 as each reader loads it: the sun light radius f8, the range test f0
void ShadowRadiusLight(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[8]); }
void ShadowRadiusRange(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[0]); }

// before the budget compare, r11 the tiles taken this frame, r29 the renderer, r27 the tile params
void GrassTileBudget(PPCContext& ctx, uint8_t*) {
    using fcip::grass::Admission;
    Admission a = fcip::grass::Admit(ctx, uint32_t(ctx.r[29]), uint32_t(ctx.r[11]), uint32_t(ctx.r[27]));
    if (a == Admission::Take) ctx.r[11] = 0;
    else if (a == Admission::Wait) ctx.r[11] = fcip::grass::kStockEntries;
}

// replaces the cache scan: both targets read r18 as entry+4, the miss clears stream r3
bool GrassTileLookup(PPCContext& ctx, uint8_t*) {
    uint32_t desc = 0;
    bool hit = fcip::grass::Lookup(ctx, uint32_t(ctx.r[29]), uint32_t(ctx.r[27]), desc);
    ctx.r[18] = desc;
    if (!hit) ctx.r[3] = 1;
    return hit;
}

// the miss path after the blade copy, before the unlock: r27 the params, r29 the renderer
void GrassTileRefilled(PPCContext& ctx, uint8_t*) { fcip::grass::Refilled(uint32_t(ctx.r[29]), uint32_t(ctx.r[27])); }
