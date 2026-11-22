// The fps counter and the loading screen's shader archive progress, drawn in the game's own fonts
#pragma once

struct PPCContext;

namespace fcip {

// each Font_Draw before it runs: notes the loading screen's label while the screen holds for the archive
void NoteFontDraw(PPCContext& ctx);

}  // namespace fcip
