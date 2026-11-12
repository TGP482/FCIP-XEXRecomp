// Draw distance multipliers, applied once a rendered frame on the level's own values so a reload or a changed
// multiplier rescales from stock instead of compounding
#include "common/draw_distance.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/aspect.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// C3DEngine
constexpr uint32_t kDetailViewDistance = 0x4C0;
constexpr uint32_t kSunSlopeScale = 0x4E8;
constexpr uint32_t kGrassViewDistance = 0xB30;
constexpr uint32_t kShadowSectorRadius = 0x1170;
constexpr uint32_t kShadowRadius = 0x1174;
constexpr uint32_t kShadowRadiusSpeed = 0x11F4;
// grass tiles (layers 5 and 9) sort far to near on view depth over this renderer value, 50 at construction; deeper
// tiles all key 0 and draw in sector order, blending farther blades over nearer ones
constexpr uint32_t kGrassSortDepth = 0x94;
// CObjManager
constexpr uint32_t kObjLodRatio = 0xF4;
constexpr uint32_t kObjViewDistRatio = 0xF8;
constexpr uint32_t kObjSpriteSwitchRatio = 0xFC;
constexpr uint32_t kStockShadowSide = 1344;
constexpr float kSectorSize = 64.0f;

bool GuestOk(uint32_t addr) { return addr >= 0x10000u && addr < 0xC0000000u; }

void WF(uint32_t addr, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(addr, raw);
}

// the game's own value, remembered so the multiplier applies to stock
struct Field {
    float stock = 0.0f;
    float applied = 0.0f;
    bool seen = false;

    // the value as the game left it, the stock one once it was ours
    float Stock(float raw) {
        if (!seen || raw != applied) {
            stock = raw;
            seen = true;
        }
        return stock;
    }

    void Set(uint32_t addr, float raw, float want) {
        if (raw != want) WF(addr, want);
        applied = want;
    }
};

void Scale(uint32_t addr, float mult, Field& f) {
    float raw = GF(addr);
    if (raw > 0.0f) f.Set(addr, raw, f.Stock(raw) * mult);
}

// adds to the game's own value, which may be 0
void Offset(uint32_t addr, float add, Field& f) {
    float raw = GF(addr);
    f.Set(addr, raw, f.Stock(raw) + add);
}

// raises the game's own value to at least floor
void AtLeast(uint32_t addr, float floor, Field& f) {
    float raw = GF(addr);
    f.Set(addr, raw, std::max(f.Stock(raw), floor));
}

Field g_detail;
Field g_grass_sort;
Field g_obj_view;
Field g_obj_lod;
Field g_obj_sprite;
Field g_sun_slope;
std::atomic<float> g_level_radius{0.0f};
std::atomic<float> g_level_detail{0.0f};

void ApplyEngine(uint32_t engine) {
    const fcip::Settings& s = fcip::Get();
    // checkpoint loads restore an engine snapshot: the level's values stand
    if (float d = g_level_detail; d > 0.0f) g_detail = {d, GF(engine + kDetailViewDistance), true};
    Scale(engine + kDetailViewDistance, float(s.detail_distance), g_detail);
    if (float r = g_level_radius; r > 0.0f && GF(engine + kShadowRadius) != r) WF(engine + kShadowRadius, r);
    // the radius stays stock, saves snapshot it; the radius readers scale it. The walk would take a minute at 12x, so
    // the radius lands the same frame; finite, the speed is multiplied by a frame time that is 0 while paused.
    WF(engine + kShadowRadiusSpeed, 1.0e6f);
    W8(engine + kShadowSectorRadius, s.shadow_distance == 1);
    // the sun texel size over stock: the slope bias is the value over 2^24, 0 at the stock texel, nearing 2 texels,
    // the reach of the 4 tap compare
    float texel = float(s.shadow_distance) * float(kStockShadowSide) / float(fcip::SunShadowSide());
    float slope = texel > 1.0f ? 2.0f * (1.0f - 1.0f / texel) : 0.0f;
    Offset(engine + kSunSlopeScale, slope * 16777216.0f, g_sun_slope);
    // past the grass distance with room for a tile's height, stock at 1x; engine+0xB30 stays stock, split screen
    // code reads it too
    uint32_t renderer = R32(SYM_Renderer_pointer);
    if (GuestOk(renderer)) {
        float grass = GF(engine + kGrassViewDistance) * float(s.grass_distance);
        AtLeast(renderer + kGrassSortDepth, s.grass_distance > 1 ? grass + 16.0f : 0.0f, g_grass_sort);
    }
}

void Apply() {
    const fcip::Settings& s = fcip::Get();
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    if (GuestOk(engine)) ApplyEngine(engine);
    uint32_t objects = R32(SYM_ObjManager_pointer);
    if (!GuestOk(objects)) return;
    Scale(objects + kObjViewDistRatio, float(s.object_distance), g_obj_view);
    Scale(objects + kObjLodRatio, float(s.model_detail), g_obj_lod);
    Scale(objects + kObjSpriteSwitchRatio, float(s.vegetation_detail), g_obj_sprite);
}

}  // namespace

namespace fcip {

void NoteLevelRadius(float radius) { g_level_radius = radius; }
void NoteLevelDetail(float detail) { g_level_detail = detail; }

// at 1x the 3x3 ring: stock asks grass only for near ones there, so a visible one outside the near set stayed bald
float GrassReach(int32_t cam_x, int32_t cam_y, int32_t x, int32_t y) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    if (!GuestOk(engine)) return -1.0f;
    float reach = GF(engine + kGrassViewDistance) * float(Get().grass_distance) / kSectorSize;
    int gx = std::max(std::abs(x - cam_x) - 1, 0);
    int gy = std::max(std::abs(y - cam_y) - 1, 0);
    float gap = float(gx * gx + gy * gy);
    return gap < reach * reach ? gap : -1.0f;
}

void ScaleGrassRadius(double& radius) { radius *= double(Get().grass_distance); }
void ScaleShadowRadius(double& radius) { radius *= double(Get().shadow_distance); }

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(C3DEngine_RenderWorld));

// once a rendered frame
PPC_FUNC(GUEST(C3DEngine_RenderWorld)) {
    Apply();
    fcip::AspectTick();
    GUEST_ORIG(C3DEngine_RenderWorld)(ctx, base);
}
