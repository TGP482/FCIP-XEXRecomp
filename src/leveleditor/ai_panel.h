// the solo level editor's AI objects in the map editor's dock (ai_panel.cpp)
#pragma once
#include <cstdint>

namespace fcip::editor {

void DrawAiPanel();           // window thread, in the ImGui frame
uint32_t SelectedAiObject();  // window thread, 0 with none
void SelectAiObject(uint32_t node);  // 0 for none
bool DeleteSelectedAiObject();       // false with none selected

}  // namespace fcip::editor
