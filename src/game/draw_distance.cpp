// default.xex: the instruction hooks of the shadow distance ([[hook]] in config/game.toml), and the level's own shadow
// radius and detail distance
#include "common/draw_distance.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kDetailViewDistance = 0x4C0;
constexpr uint32_t kShadowRadius = 0x1174;

}  // namespace

// engine+0x1174 as each reader loads it: the walk target f0, the sun light radius f8, the range test f1
void ShadowRadiusTarget(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[0]); }
void ShadowRadiusLight(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[8]); }
void ShadowRadiusRange(PPCContext& ctx, uint8_t*) { fcip::ScaleShadowRadius(ctx.f[1]); }

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
