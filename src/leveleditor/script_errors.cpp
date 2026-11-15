// ige.xex, the solo level editor: the engine calls most scripts unprotected, and Lua exits on an error nothing
// catches, so every script error's text is logged before it is raised
#include <string>

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

using namespace kernel;

PPC_EXTERN_FUNC(__imp__sub_823DC9F0);

// Lua raises an error: r3 the state, r4 the message, null when it is already on the stack
PPC_FUNC(sub_823DC9F0) {
    if (uint32_t message = uint32_t(ctx.r[4])) LOGE("script error: %s", ReadCString(message).c_str());
    __imp__sub_823DC9F0(ctx, base);
}
