// User maps over system link. Joining a session on a user map this PC lacks no longer stops at USER MAP
// DOES NOT EXIST: the join goes on, and as the host's map loads the game's own map download (82228648, reached from
// 821451C0 in a network game) fetches it from the host into maps\play.
#include <string>

#include "core/log.h"
#include "kernel/kernel.h"

using namespace kernel;

constexpr uint32_t kMapNameChars = 32;

// the join's lookup of the session's user map: r10 found, r27 its name. A missing one joins as a found one would,
// minus the loading the download does later
bool JoinMissingUserMap(PPCContext& ctx, uint8_t*) {
    if (uint32_t(ctx.r[10])) return false;
    std::string name = ReadCString(uint32_t(ctx.r[27]), kMapNameChars);
    LOGR("user maps: joining a session on %s, which the host sends as the map loads", name.c_str());
    return true;
}
