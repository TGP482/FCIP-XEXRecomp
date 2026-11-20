// Frame rate fixes: what the game tied to its 30 fps console frame now follows the frame time.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "game/fpsfixes.h"
#include "xdk/hle.h"

using namespace kernel;

// Frame clock

// The game's frame clock. Its time source rounds to whole milliseconds, so at high frame rates every frame time is
// off by up to a millisecond; it now reads the same clock unrounded. The menus and the UI step in whole milliseconds,
// truncating each frame's time, so the part each frame drops is carried to the next.

namespace {

// the millisecond clock: base milliseconds, start ticks and ticks per second
constexpr uint32_t kClockBaseMs = 0x0;
constexpr uint32_t kClockStart = 0x10;
constexpr uint32_t kClockFrequency = 0x18;
constexpr uint32_t kTimerFrame = 0x68;  // the timer's frame time, double seconds

float g_frame_ms = 0.0f;  // this frame's whole milliseconds for the menus and the UI

double GuestDouble(uint32_t va) {
    uint64_t raw = R64(va);
    double value;
    std::memcpy(&value, &raw, 8);
    return value;
}

}  // namespace

double fcip::FrameTime() { return GuestDouble(SYM_Timer + kTimerFrame); }

PPC_EXTERN_FUNC(GUEST_ORIG(Timer_Update));

// the timer's time source, seconds since start
PPC_FUNC(GUEST(Timer_Now)) {
    uint32_t clock = uint32_t(CallGuest(ctx, SYM_Clock_Get, {}));
    CallGuest(ctx, SYM_Clock_Ms, {clock});  // steps its drift correction on the base
    int64_t ticks = int64_t(ppc_timebase()) - int64_t(R64(clock + kClockStart));
    int64_t frequency = int64_t(R64(clock + kClockFrequency));
    double ms = double(R32(clock + kClockBaseMs));
    if (frequency > 0) ms += double(ticks) * 1000.0 / double(frequency);
    ctx.f[1] = ms * 0.001;
}

// once a frame, sets the frame time
PPC_FUNC(GUEST(Timer_Update)) {
    GUEST_ORIG(Timer_Update)(ctx, base);
    static double carry = 0.0;
    double ms = fcip::FrameTime() * 1000.0 + carry;
    double whole = std::floor(ms);
    carry = ms - whole;
    g_frame_ms = float(whole);
}

// the frame time in milliseconds just before the menus and the UI truncate it
void WholeFrameMsF10(PPCContext& ctx, uint8_t*) { ctx.f[10] = g_frame_ms; }
void WholeFrameMsF13(PPCContext& ctx, uint8_t*) { ctx.f[13] = g_frame_ms; }

// Animation

// A model's animation advances in whole 1/120 s steps, the rest carried to its next update. Above 120 fps frames then
// alternate one step and none, so poses and root motion moved in jerks. Those frames now advance by their own time.

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

// Step ups

// A living entity's position query eases the shown height up a step by at least 0.075 per call, a rate tuned to the
// console's 30 fps; at high frame rates the step snapped. The least rise now follows the frame time.

// f12 = 0.075 just loaded
void StepUpRate(PPCContext& ctx, uint8_t*) { ctx.f[12] *= fcip::FrameTime() / fcip::kConsoleFrame; }

// Continuous emitters

// The 3D engine's continuous emitters spawn their particle count once a frame, so at high frame rates they made many
// times the console's particles. Each frame's count now follows the frame time, the fraction carried per emitter.

namespace {

constexpr uint32_t kEmitterParams = 0x8;
constexpr uint32_t kParamsCount = 0xD4;  // particles per spawn

std::unordered_map<uint32_t, double> g_carry;  // per emitter
uint32_t g_params = 0;  // whose count is changed for the spawn call
uint32_t g_count = 0;

}  // namespace

// before the spawn call: r3 the emitter, f1 the frame time
void ScaleEmitterCount(PPCContext& ctx, uint8_t*) {
    double frame = ctx.f[1];
    if (frame >= fcip::kConsoleFrame) return;  // the spawn thins itself below 30 fps
    uint32_t emitter = uint32_t(ctx.r[3]);
    g_params = R32(emitter + kEmitterParams);
    g_count = R32(g_params + kParamsCount);
    double& carry = g_carry[emitter];
    double count = g_count * frame / fcip::kConsoleFrame + carry;
    double whole = std::floor(count);
    carry = count - whole;
    W32(g_params + kParamsCount, uint32_t(whole));
}

// after the spawn call, or past it for an emitter out of view
void RestoreEmitterCount(PPCContext&, uint8_t*) {
    if (!g_params) return;
    W32(g_params + kParamsCount, g_count);
    g_params = 0;
}

// AI path arrival

// An AI following a path also arrives once it starts moving away from its point within two frames of travel, a
// window sized by the frame time. At high frame rates the window shrank until a blocked AI never arrived and walked on
// the spot. The window keeps the console's frame as its least.

// f1 = the frame time just returned, before speed times 2
void ArrivalFrameTime(PPCContext& ctx, uint8_t*) { ctx.f[1] = std::max(ctx.f[1], fcip::kConsoleFrame); }

// Loading screen spinner

// The loading screen's spinner turns 6 degrees each draw, which the console drew at 30 fps at most. At high frame
// rates it spun as fast as the screen redrew. Each draw now turns by its share of a console frame of real time, as
// the loading screen draws outside the game's frame clock.

// f13 = 6.0, before the spinner's angle adds it
void LoadingIconTurn(PPCContext& ctx, uint8_t*) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last;
    Clock::time_point now = Clock::now();
    double frames = std::chrono::duration<double>(now - last).count() / fcip::kConsoleFrame;
    last = now;
    // a draw slower than the console's frame still turns one whole step, as it did there
    ctx.f[13] = float(ctx.f[13] * std::min(frames, 1.0));
}

// Underwater bob

// Swimming below p_waterbob_mindepth bobs the view: every update adds a sine of the bob phase to the pitch and roll
// the last update left, so the swing grew with the frame rate. Each update now adds its share of a console frame,
// and the roll's pull back to level near the surface follows the frame time too.

namespace {

double ConsoleFrames() { return fcip::FrameTime() / fcip::kConsoleFrame; }

}  // namespace

// f29 = p_waterbob_pitch, before the pitch adds sine times depth times it
void WaterBobPitch(PPCContext& ctx, uint8_t*) { ctx.f[29] = float(ctx.f[29] * ConsoleFrames()); }

// f28 = p_waterbob_roll, f30 = depth past p_waterbob_mindepth in 0..1, before the roll adds and scales by depth
void WaterBobRoll(PPCContext& ctx, uint8_t*) {
    double frames = ConsoleFrames();
    ctx.f[28] = float(ctx.f[28] * frames);
    ctx.f[30] = float(std::pow(ctx.f[30], frames));
}
