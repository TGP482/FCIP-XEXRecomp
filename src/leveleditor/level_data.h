// the solo level editor's Level tab: the kept LevelData.xml's water level and terrain surface types (level_data.cpp)
#pragma once

struct PPCContext;

namespace fcip::editor {

void DrawLevelPanel();                 // window thread
void UpdateLevelData(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any

}  // namespace fcip::editor
