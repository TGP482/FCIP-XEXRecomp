// default.xex: the instruction hooks of the draw distance and the grass cache ([[hook]] in config/game.toml), sector
// streaming within the grass distance, the level's own shadow radius and detail distance, and every object casting and
// receiving ray traced ultra shadows, and casting into the volumetric fog's sun map
#include <algorithm>
#include <limits>
#include <vector>

#include "common/draw_distance.h"
#include "common/grass_tiles.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/occlusion.h"
#include "game/grass_blocks.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kDetailViewDistance = 0x4C0;
constexpr uint32_t kShadowRadius = 0x1174;
// sector +0x149: 0x08 render data loaded, 0x10 drawn, 0x40 detail asked, 0x80 detail loaded
constexpr uint32_t kSectorFlags = 0x149;
constexpr uint8_t kRenderLoaded = 0x08;
constexpr uint8_t kDetailAsked = 0x40;
constexpr uint32_t kNewSectors = 4;

// new sectors a pass takes; limit: the kNewSectors nearest of those the last pass turned away
struct Gate {
    uint32_t left = kNewSectors;
    float limit = std::numeric_limits<float>::max();
    std::vector<float> waiting;

    void Start() {
        left = kNewSectors;
        limit = std::numeric_limits<float>::max();
        if (waiting.size() > kNewSectors) {
            std::nth_element(waiting.begin(), waiting.begin() + (kNewSectors - 1), waiting.end());
            limit = waiting[kNewSectors - 1];
        }
        waiting.clear();
    }

    bool Take(float gap) {
        if (left && gap <= limit) {
            left--;
            return true;
        }
        waiting.push_back(gap);
        return false;
    }
};

Gate g_kept;
uint32_t g_last_sector = 0xFFFFFFFFu;

int32_t S32(PPCContext& ctx, int n) { return int32_t(ctx.r[n]); }

}  // namespace

// f24 = engine+0xB30, squared for the tile loop's radius and stored to params+0x1C
void GrassRadius(PPCContext& ctx, uint8_t*) { fcip::ScaleGrassRadius(ctx.f[24]); }

// engine+0x1174 as each reader loads it: the walk target f0, the sun light radius f8, the range test f1
void ShadowRadiusTarget(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[0]); }
void ShadowRadiusLight(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[8]); }
void ShadowRadiusRange(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[1]); }

// Sector streaming reads each sector's byte in the camera sector's sight table: bit 1 the near set (3x3 to 5x5), bit
// 8 the visible set, the level's PVS, often under 5 sectors. Near and visible sectors get the render list, which draws
// their terrain and grass, and only near ones in the 3x3 get detail data (grass tiles). Within the grass distance near
// and visible sectors now keep their detail data too. A pass adds at most kNewSectors, nearest first.

// after the ring test: r11 keep, r28 far, r29 near, r20/r21 the camera sector's row/column, r25/r27 this sector's, r30
// the sector; sectors run in order, so a lower one starts a pass
void GrassSectorRing(PPCContext& ctx, uint8_t*) {
    uint32_t key = uint32_t(ctx.r[25]) << 16 | (uint32_t(ctx.r[27]) & 0xFFFF);
    if (key <= g_last_sector) g_kept.Start();
    g_last_sector = key;
    uint8_t flags = R8(uint32_t(ctx.r[30]) + kSectorFlags);
    bool asked = flags & kDetailAsked;
    // render data landing later zeroes the detail header (825FDF68), so detail waits for it
    if (!asked && !(flags & kRenderLoaded)) {
        ctx.r[11] = 0;
        return;
    }
    if (ctx.r[11] || !(ctx.r[28] || ctx.r[29])) return;
    float gap = fcip::GrassReach(S32(ctx, 21), S32(ctx, 20), S32(ctx, 27), S32(ctx, 25));
    if (gap >= 0.0f && (asked || g_kept.Take(gap))) ctx.r[11] = 1;
}

// before the budget compare, r11 the tiles taken this frame, r30 the renderer, r27 the tile params: a zero count lets
// the tile through, 512 skips it
void GrassTileBudget(PPCContext& ctx, uint8_t*) {
    using fcip::grass::Admission;
    if (fcip::grass_blocks::Owns(uint32_t(ctx.r[27]))) {
        ctx.r[11] = 0;
        return;
    }
    Admission a = fcip::grass::Admit(ctx, uint32_t(ctx.r[30]), uint32_t(ctx.r[11]), uint32_t(ctx.r[27]));
    if (a == Admission::Take) ctx.r[11] = 0;
    else if (a == Admission::Wait) ctx.r[11] = fcip::grass::kStockEntries;
}

// replaces the cache scan: both targets read r23 as entry+4, the miss clears stream 1 from r4 to r7
bool GrassTileLookup(PPCContext& ctx, uint8_t*) {
    if (fcip::grass_blocks::Owns(uint32_t(ctx.r[27]))) {
        ctx.r[23] = fcip::grass_blocks::Desc();
        return true;
    }
    uint32_t desc = 0;
    bool hit = fcip::grass::Lookup(ctx, uint32_t(ctx.r[30]), uint32_t(ctx.r[27]), desc);
    ctx.r[23] = desc;
    if (!hit) {
        ctx.r[4] = 1;
        ctx.r[5] = 0;
        ctx.r[6] = 0;
        ctx.r[7] = 0;
    }
    return hit;
}

// the miss path after the blade copy, before the unlock: r27 the params, r30 the renderer
void GrassTileRefilled(PPCContext& ctx, uint8_t*) { fcip::grass::Refilled(uint32_t(ctx.r[30]), uint32_t(ctx.r[27])); }

PPC_EXTERN_FUNC(__imp__sub_825E71A0);
PPC_EXTERN_FUNC(__imp__sub_820F36D8);

// the level XML's environment at level load, before any checkpoint restore, r3 the engine
PPC_FUNC(sub_825E71A0) {
    uint32_t engine = uint32_t(ctx.r[3]);
    __imp__sub_825E71A0(ctx, base);
    fcip::NoteLevelRadius(GF(engine + kShadowRadius));
    fcip::NoteLevelDetail(GF(engine + kDetailViewDistance));
}

// Game:SetDynamicShadowRadius, the EnvironmentTrigger shadow triggers (21_DUNEA, 24_SHANTYDAY)
PPC_FUNC(sub_820F36D8) {
    __imp__sub_820F36D8(ctx, base);
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    if (engine >= 0x10000u && engine < 0xC0000000u) fcip::NoteLevelRadius(GF(engine + kShadowRadius));
}

// Ray traced ultra shadows reach every object. The two sector gathers, 825D64A0 and 825FAC90, draw a node into a
// shadow pass only when its render flags (vtable +0x5C) hold CastShadowMaps, the entity XML's flag 4: vehicles and
// dropped weapons leave it clear. A render item's light set (82387B68) samples the shadow map only when the item's
// flags (+0x44) lack 0x40, which vegetation such as bushes sets. First person items (flag 1, the weapon and arms
// 82502F48 draws in the near depth range) keep the game's choice: the player's shadow body (carver_shadow.xbg) stands
// around the camera and would shade them. The volumetric fog's own run of the sun map takes every object too, so its
// shafts stop at solid objects and pass only through gaps such as leaves.
namespace {

constexpr uint64_t kCastShadowMaps = 4;
constexpr uint64_t kFirstPerson = 1;

bool RayTracedUltra() { return fcip::Get().shadow_filter == fcip::kFilterRayUltra && gpu::RayTracing(); }

}  // namespace

// r3 = the node's render flags, before the shadow pass tests kCastShadowMaps
void CastShadowMaps(PPCContext& ctx, uint8_t*) {
    if (RayTracedUltra() || fcip::FogMapRun()) ctx.r[3] |= kCastShadowMaps;
}

// r5 = the light set's shadow enabled byte, just taken from the item's flags in r7
void ReceiveShadowMaps(PPCContext& ctx, uint8_t*) {
    bool first_person = ctx.r[7] & kFirstPerson;
    if (RayTracedUltra() && !first_person) ctx.r[5] = 1;
}
