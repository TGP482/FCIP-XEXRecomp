// default.xex: the level's own detail distance
#include "common/draw_distance.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kDetailViewDistance = 0x4C0;

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_825E71A0);

// the level XML's environment at level load, before any checkpoint restore, r3 the engine
PPC_FUNC(sub_825E71A0) {
    uint32_t engine = uint32_t(ctx.r[3]);
    __imp__sub_825E71A0(ctx, base);
    fcip::NoteLevelDetail(GF(engine + kDetailViewDistance));
}
