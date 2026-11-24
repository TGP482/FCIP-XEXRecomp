// Frame rate fixes: what the game tied to its 30 fps console frame now follows the frame time.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "common/fpsfixes.h"
#include "xdk/hle.h"

using namespace kernel;

// the editor's build keeps some hooked values in other registers
constexpr bool kEditor = std::string_view(IMAGE_XEX) == "ige.xex";

// Frame clock

// The game's frame clock. Its time source rounds to whole milliseconds, so at high frame rates every frame time is
// off by up to a millisecond; it now reads the same clock unrounded. The menus and the UI step in whole milliseconds,
// truncating each frame's time, so the part each frame drops is carried to the next.

// the millisecond clock: base milliseconds, start ticks and ticks per second
constexpr uint32_t kClockBaseMs = 0x0;
constexpr uint32_t kClockStart = 0x10;
constexpr uint32_t kClockFrequency = 0x18;
constexpr uint32_t kTimerFrame = 0x68;  // the timer's frame time, double seconds

static float g_frame_ms = 0.0f;  // this frame's whole milliseconds for the menus and the UI
static double g_game_seconds = 0.0;  // the frame times added up

static double GuestDouble(uint32_t va) {
    uint64_t raw = R64(va);
    double value;
    std::memcpy(&value, &raw, 8);
    return value;
}

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
    g_game_seconds += fcip::FrameTime();
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

constexpr float kStep = 1.0f / 120.0f;  // the MORE type's step, set by its constructor 82447C30
// the model's animation: +0x18 its clock, whose +0x24 is the carried time; +0x20 set once this update stepped
constexpr uint32_t kAnimationClock = 0x18;
constexpr uint32_t kClockCarry = 0x24;
constexpr uint32_t kAnimationStepped = 0x20;

static void WF(uint32_t va, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(va, raw);
}

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

// Small moves

// A living entity's step skips its move when the move is under 1 mm, a cut off tuned to the console's 30 fps steps.
// At high frame rates slow moves fell under it every step and were lost, so air control never pushed the player off a
// ledge's edge it hung on. The cut off now follows the step's time.

// f0 = 1e-6 just loaded, the least squared move; r22 the step's time
void LeastMove(PPCContext& ctx, uint8_t*) {
    double frames = std::min(GF(uint32_t(ctx.r[22])) / fcip::kConsoleFrame, 1.0);
    ctx.f[0] *= frames * frames;
}

// Ladder exits

// Climbing past a ladder's top lets go of it, and the ladder's script runs every frame the player still overlaps it,
// grabbing it again when the stick pushes on from the ledge side. At 30 fps the first step after letting go took the
// player out of the overlap, so the script stayed quiet until the player had landed away from the ladder; at high frame
// rates the overlap lasted many frames and the script grabbed again, so the player got stuck at the top. Grabs are now
// skipped from letting go at the top until the script has gone a console frame without asking.

constexpr int kExitPlayer = kEditor ? 30 : 29;

static uint32_t g_exit_player = 0;
static double g_hold_until = 0.0;

// the top exit's let go, kExitPlayer the player
void LadderTopExit(PPCContext& ctx, uint8_t*) {
    g_exit_player = uint32_t(ctx.r[kExitPlayer]);
    g_hold_until = g_game_seconds + fcip::kConsoleFrame;
}

PPC_EXTERN_FUNC(GUEST_ORIG(Player_SetLadder));

// (player, on, ladder id, ...)
PPC_FUNC(GUEST(Player_SetLadder)) {
    bool grab = (ctx.r[4] & 0xFF) != 0;
    bool held = uint32_t(ctx.r[3]) == g_exit_player && g_game_seconds < g_hold_until;
    if (grab && held) {
        g_hold_until = g_game_seconds + fcip::kConsoleFrame;
        return;
    }
    GUEST_ORIG(Player_SetLadder)(ctx, base);
}

// Continuous emitters

// The 3D engine's continuous emitters spawn their particle count once a frame, so at high frame rates they made many
// times the console's particles. Each frame's count now follows the frame time, the fraction carried per emitter.

constexpr uint32_t kEmitterParams = 0x8;
constexpr uint32_t kParamsCount = 0xD4;  // particles per spawn

static std::unordered_map<uint32_t, double> g_carry;  // per emitter
static uint32_t g_params = 0;  // whose count is changed for the spawn call
static uint32_t g_count = 0;

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

// Loading screen

// The loading screen's spinner turns 6 degrees each draw, and its fade out drops the alpha by 10 each draw, which the
// console drew at 30 fps at most. At high frame rates both ran as fast as the screen redrew. Each draw now moves by
// its share of a console frame of real time, as the loading screen draws outside the game's frame clock.

using Clock = std::chrono::steady_clock;

constexpr int32_t kFadeStart = 250;  // the fade's first alpha, set by 82156810
constexpr int32_t kFadeStep = 10;

// console frames of real time since the last draw; a draw slower than the console's frame still moves one whole
// step, as it did there
static double DrawFrames(Clock::time_point& last) {
    Clock::time_point now = Clock::now();
    double frames = std::chrono::duration<double>(now - last).count() / fcip::kConsoleFrame;
    last = now;
    return std::min(frames, 1.0);
}

// f13 = 6.0, before the spinner's angle adds it
void LoadingIconTurn(PPCContext& ctx, uint8_t*) {
    static Clock::time_point last;
    ctx.f[13] = float(ctx.f[13] * DrawFrames(last));
}

// r31 = the fade's alpha just stepped down by 10, after the draw at the alpha before
void LoadingFadeStep(PPCContext& ctx, uint8_t*) {
    static Clock::time_point last;
    static double carry = 0.0;
    int32_t before = int32_t(ctx.r[31]) + kFadeStep;
    if (before == kFadeStart) carry = 0.0;
    double step = kFadeStep * DrawFrames(last) + carry;
    double whole = std::floor(step);
    carry = step - whole;
    ctx.r[31] = uint64_t(int64_t(before - int32_t(whole)));
}

// Underwater bob

// Swimming below p_waterbob_mindepth bobs the view: every update adds a sine of the bob phase to the pitch and roll
// the last update left, so the swing grew with the frame rate. Each update now adds its share of a console frame,
// and the roll's pull back to level near the surface follows the frame time too.

constexpr int kBobPitch = kEditor ? 27 : 29;  // p_waterbob_pitch
constexpr int kBobRoll = kEditor ? 26 : 28;   // p_waterbob_roll
constexpr int kBobDepth = kEditor ? 31 : 30;  // depth past p_waterbob_mindepth in 0..1

static double ConsoleFrames() { return fcip::FrameTime() / fcip::kConsoleFrame; }

// before the pitch adds sine times depth times p_waterbob_pitch
void WaterBobPitch(PPCContext& ctx, uint8_t*) { ctx.f[kBobPitch] = float(ctx.f[kBobPitch] * ConsoleFrames()); }

// before the roll adds p_waterbob_roll's swing and scales it by depth
void WaterBobRoll(PPCContext& ctx, uint8_t*) {
    double frames = ConsoleFrames();
    ctx.f[kBobRoll] = float(ctx.f[kBobRoll] * frames);
    ctx.f[kBobDepth] = float(std::pow(ctx.f[kBobDepth], frames));
}

// Virtual keyboard

#if HAS_VirtualKeyboard_Update  // the editor never opens it

// The virtual keyboard's idle animation steps one of its 360 frames each update, a rate tuned to the console's 30 fps;
// at high frame rates it cycled many times a second and the page shook. It now steps by console frames of time.

constexpr uint32_t kKeyboardIdleFrame = 0x118;  // the frame it shows next, 0 to 359
constexpr double kKeyboardIdleFrames = 360.0;

PPC_EXTERN_FUNC(GUEST_ORIG(VirtualKeyboard_Update));

// (keyboard)
PPC_FUNC(GUEST(VirtualKeyboard_Update)) {
    static double frame = 0.0;
    frame = std::fmod(frame + ConsoleFrames(), kKeyboardIdleFrames);
    W32(uint32_t(ctx.r[3]) + kKeyboardIdleFrame, uint32_t(frame));
    GUEST_ORIG(VirtualKeyboard_Update)(ctx, base);
}
#endif

// Boat drag

// A boat's water drag takes a share of its forward and sideways speed each physics step, Dampv and Dampvs, a share
// tuned to the 0.02 s steps the physics cuts the console's 30 fps frames into. Above 50 fps every frame is one shorter
// step, so boats lost that share many more times a second and crawled. Each step now takes what 0.02 s steps would
// over its own time.

constexpr double kBoatStep = 0.02;  // the physics step a 30 fps frame is cut into

static float StepShare(double share, double step) {
    return float(1.0 - std::pow(std::max(1.0 - share, 0.0), step / kBoatStep));
}

// f9 = Dampv and f10 = Dampvs just loaded, f29 the step's time
void BoatDrag(PPCContext& ctx, uint8_t*) {
    ctx.f[9] = StepShare(ctx.f[9], ctx.f[29]);
    ctx.f[10] = StepShare(ctx.f[10], ctx.f[29]);
}
