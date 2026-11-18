// default.xex: the engine calls most scripts unprotected, and Lua exits on an error nothing catches, so every script
// error's text is logged as Lua reports it, a custom level's Mission.lua mistakes included
#include <string>

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

using namespace kernel;

PPC_EXTERN_FUNC(__imp__sub_823DF858);

// Lua reports an error: r3 the state, r4 the message
PPC_FUNC(sub_823DF858) {
    if (uint32_t message = uint32_t(ctx.r[4])) LOGE("script error: %s", ReadCString(message).c_str());
    __imp__sub_823DF858(ctx, base);
}
