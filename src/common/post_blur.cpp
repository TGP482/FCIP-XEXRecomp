// The post effects' blur (default.xex 8237B268, ige.xex 824815E8), under the glow and the moonlight: two passes of
// nine taps whole title texels apart, marked so the renderer widens each tap over an upscaled resolve
#include "d3d/occlusion.h"
#include "xdk/hle.h"

using namespace kernel;

#if HAS_PostEffect_Blur && HAS_DisplayBlock
constexpr uint32_t kBlockDevice = 0x14;  // the display block's device

PPC_EXTERN_FUNC(GUEST_ORIG(PostEffect_Blur));

PPC_FUNC(GUEST(PostEffect_Blur)) {
    uint32_t dev = R32(SYM_DisplayBlock + kBlockDevice);
    d3d::Blur(dev, true);
    GUEST_ORIG(PostEffect_Blur)(ctx, base);
    d3d::Blur(dev, false);
}
#endif
