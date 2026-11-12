// The 3D engine's continuous emitters spawn their particle count once a frame, so at high frame rates they made many
// times the console's particles. Each frame's count now follows the frame time, the fraction carried per emitter.
#include <cmath>
#include <unordered_map>

#include "game/frame_time.h"
#include "xdk/hle.h"

using namespace kernel;

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
