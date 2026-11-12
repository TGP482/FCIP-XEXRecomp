// A model's animation advances in whole 1/120 s steps, the rest carried to its next update. Above 120 fps frames then
// alternate one step and none, so poses and root motion moved in jerks. Those frames now advance by their own time.
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr float kStep = 1.0f / 120.0f;  // the MORE type's step, set by its constructor 82447C30
// the model's animation: +0x18 its clock, whose +0x24 is the carried time; +0x20 set once this update stepped
constexpr uint32_t kAnimationClock = 0x18;
constexpr uint32_t kClockCarry = 0x24;
constexpr uint32_t kAnimationStepped = 0x20;

void WF(uint32_t va, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(va, raw);
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Animation_Step));

// (animation, model, float* frame time): rounds the frame time down to whole steps
PPC_FUNC(GUEST(Animation_Step)) {
    uint32_t animation = uint32_t(ctx.r[3]);
    uint32_t time = uint32_t(ctx.r[5]);
    if (GF(time) >= kStep) {
        GUEST_ORIG(Animation_Step)(ctx, base);
        return;
    }
    uint32_t carry = R32(animation + kAnimationClock) + kClockCarry;
    WF(time, GF(time) + GF(carry));
    WF(carry, 0.0f);
    W8(animation + kAnimationStepped, 1);
}
