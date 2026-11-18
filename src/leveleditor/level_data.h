// the solo level editor's Level tab: the kept LevelData.xml's water level and terrain surface types (level_data.cpp)
#pragma once
#include <string>


struct PPCContext;

namespace fcip::editor {

void DrawLevelPanel();                 // window thread
void UpdateLevelData(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any
// a LevelInfo value: read from the snapshot and set through the queue on the window thread, read on the editor
// thread for the save; empty when the level keeps no LevelData or has no such value
std::string LevelInfoValue(const std::string& key);
void SetLevelInfoValue(const std::string& key, const std::string& value);
std::string KeptLevelInfo(const std::string& key);

}  // namespace fcip::editor
