// the solo level editor's Environment tab: the kept mission's Environment, applied as it loads (environment.cpp)
#pragma once

struct PPCContext;

namespace fcip::editor {

void DrawEnvironmentPanel();             // window thread
void UpdateEnvironment(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any

}  // namespace fcip::editor
