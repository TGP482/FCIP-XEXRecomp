// ige.xex, the solo level editor: a level stands still while it is edited, as in a level editor's edit mode. The map
// editor keeps the game running under its tools, so a campaign level's AI walked their paths, its triggers and
// sequences played the story intro and dialogue, and physics settled as the level was edited. The system update
// (82313240, which the game's update calls with pause mode 0) gets pause mode 1 while editing, which skips physics
// (82312D30), the AI (821C8A88) and the entity system (82312EC0); play test runs them as the game does.
#include "editor/tools.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

namespace {

constexpr uint64_t kPauseSimulation = 1;

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_82313240);

// the system update (r3 update flags, r4 pause mode), once a frame
PPC_FUNC(sub_82313240) {
    if (fcip::editor::g_solo && fcip::editor::ShellUp()) ctx.r[4] = kPauseSimulation;
    __imp__sub_82313240(ctx, base);
}
