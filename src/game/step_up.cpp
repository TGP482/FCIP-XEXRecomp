// A living entity's position query eases the shown height up a step by at least 0.075 per call, a rate tuned to the
// console's 30 fps; at high frame rates the step snapped. The least rise now follows the frame time.
#include "game/frame_time.h"
#include "xdk/hle.h"

// f12 = 0.075 just loaded
void StepUpRate(PPCContext& ctx, uint8_t*) { ctx.f[12] *= fcip::FrameTime() / fcip::kConsoleFrame; }
