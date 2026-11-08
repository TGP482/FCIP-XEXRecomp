// Draw distance multipliers, applied once a rendered frame on the level's own values so a reload or a changed
// multiplier rescales from stock instead of compounding
#include "common/draw_distance.h"

#include <atomic>
#include <cstring>

#include "common/aspect.h"
#include "common/settings.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// C3DEngine
constexpr uint32_t kDetailViewDistance = 0x4C0;
// CObjManager
constexpr uint32_t kObjLodRatio = 0xF4;
constexpr uint32_t kObjViewDistRatio = 0xF8;
constexpr uint32_t kObjSpriteSwitchRatio = 0xFC;

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

Field g_detail;
Field g_obj_view;
Field g_obj_lod;
Field g_obj_sprite;
std::atomic<float> g_level_detail{0.0f};

void ApplyEngine(uint32_t engine) {
    const fcip::Settings& s = fcip::Get();
    // checkpoint loads restore an engine snapshot: the level's values stand
    if (float d = g_level_detail; d > 0.0f) g_detail = {d, GF(engine + kDetailViewDistance), true};
    Scale(engine + kDetailViewDistance, float(s.detail_distance), g_detail);
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

void NoteLevelDetail(float detail) { g_level_detail = detail; }

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(C3DEngine_RenderWorld));

// once a rendered frame
PPC_FUNC(GUEST(C3DEngine_RenderWorld)) {
    Apply();
    fcip::AspectTick();
    GUEST_ORIG(C3DEngine_RenderWorld)(ctx, base);
}
