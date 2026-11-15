// ige.xex, the solo level editor: ige makes the local player only in first person (820C5548), while a campaign
// level's entities run their logic from the moment it loads. The game's script methods that draw on the local
// player's HUD then return nothing (a Waypoint's OnInit sets its objective), and an entity's event goes unheard by
// a local player that is not there. In default.xex the local player is always there.
#include "kernel/kernel.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

bool NoLocalPlayer(PPCContext& ctx) {
    if (!fcip::editor::g_solo) return false;
    uint32_t game = R32(SYM_Game_pointer);
    return game && !CallGuest(ctx, SYM_Game_LocalPlayer, {game});
}

}  // namespace

// each returns no values to the script, as on a wrong argument count
#define HUD_METHOD(sub)                       \
    PPC_EXTERN_FUNC(__imp__##sub);            \
    PPC_FUNC(sub) {                           \
        if (NoLocalPlayer(ctx)) {             \
            ctx.r[3] = 0;                     \
            return;                           \
        }                                     \
        __imp__##sub(ctx, base);              \
    }

HUD_METHOD(sub_82218448)  // SetObjectiveStatus
HUD_METHOD(sub_822185B8)  // SetObjectiveBlinking
HUD_METHOD(sub_82219ED0)  // DrawDamageBorderExplosion
HUD_METHOD(sub_82219D30)  // DrawDamageBorderHit
HUD_METHOD(sub_8221FB68)  // AddEntityToRadar
HUD_METHOD(sub_8221FC68)  // RemoveEntityFromRadar
HUD_METHOD(sub_82220A70)  // ShowPredatorHudRadioIcon

PPC_EXTERN_FUNC(__imp__sub_82131FB8);

// an entity's event as the local player hears it (r3 the entity, from 82128B08)
PPC_FUNC(sub_82131FB8) {
    if (NoLocalPlayer(ctx)) return;
    __imp__sub_82131FB8(ctx, base);
}
