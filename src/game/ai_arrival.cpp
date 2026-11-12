// An AI following a path also arrives once it starts moving away from its point within two frames of travel, a
// window sized by the frame time. At high frame rates the window shrank until a blocked AI never arrived and walked on
// the spot. The window keeps the console's frame as its least.
#include <algorithm>

#include "game/frame_time.h"
#include "xdk/hle.h"

// f1 = the frame time just returned, before speed times 2
void ArrivalFrameTime(PPCContext& ctx, uint8_t*) { ctx.f[1] = std::max(ctx.f[1], fcip::kConsoleFrame); }
