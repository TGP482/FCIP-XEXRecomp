// ige.xex, the solo level editor: the game's script methods that draw on the local player's HUD return nothing while
// there is no local player. ige brings the player in after a level's entities, and a campaign mission's entities
// already call them as they spawn (a Waypoint's OnInit sets its objective). In default.xex the player is always there.
#include "kernel/kernel.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kScriptGame = 0x0C;  // the Game script object's game

// r3 the script object
bool NoLocalPlayer(PPCContext& ctx) {
    if (!fcip::editor::g_solo) return false;
    uint32_t game = R32(uint32_t(ctx.r[3]) + kScriptGame);
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
