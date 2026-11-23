// User maps over system link. Joining a session on a user map this PC lacks no longer stops at USER MAP
// DOES NOT EXIST: the join goes on, and as the host's map loads the game's own map download (82228648, reached from
// 821451C0 in a network game) fetches it from the host into maps\play.
#include "kernel/kernel.h"

// the join's lookup of the session's user map: r10 found. A missing one joins as a found one would, minus the loading
// the download does later
bool JoinMissingUserMap(PPCContext& ctx, uint8_t*) { return uint32_t(ctx.r[10]) == 0; }
