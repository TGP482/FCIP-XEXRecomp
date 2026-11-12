// The game's frame clock. Its time source rounds to whole milliseconds, so at high frame rates every frame time is
// off by up to a millisecond; it now reads the same clock unrounded. The menus and the UI step in whole milliseconds,
// truncating each frame's time, so the part each frame drops is carried to the next.
#include <cmath>
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

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
    double ms = GuestDouble(SYM_Timer + kTimerFrame) * 1000.0 + carry;
    double whole = std::floor(ms);
    carry = ms - whole;
    g_frame_ms = float(whole);
}

// the frame time in milliseconds just before the menus and the UI truncate it
void WholeFrameMsF10(PPCContext& ctx, uint8_t*) { ctx.f[10] = g_frame_ms; }
void WholeFrameMsF13(PPCContext& ctx, uint8_t*) { ctx.f[13] = g_frame_ms; }
