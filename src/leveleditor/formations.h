// the solo level editor's Formations tab: the kept mission's AI formations and their templates (formations.cpp)
#pragma once

struct PPCContext;

namespace fcip::editor {

void DrawFormationsPanel();             // window thread
void UpdateFormations(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any

}  // namespace fcip::editor
