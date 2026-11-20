// The post effects' blur (8237B268), under the glow (82382D78) and the moonlight (823843F0): two passes of nine taps
// whole title texels apart, marked so the renderer widens each tap over an upscaled resolve
#include "d3d/occlusion.h"
#include "xdk/hle.h"

using namespace kernel;

#if HAS_PostEffect_Blur && HAS_Device_pointer
PPC_EXTERN_FUNC(GUEST_ORIG(PostEffect_Blur));

PPC_FUNC(GUEST(PostEffect_Blur)) {
    uint32_t dev = R32(SYM_Device_pointer);
    d3d::Blur(dev, true);
    GUEST_ORIG(PostEffect_Blur)(ctx, base);
    d3d::Blur(dev, false);
}
#endif
