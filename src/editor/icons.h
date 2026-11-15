// the map editor's toolbar icons, drawn from lines and shapes
#pragma once

#include <imgui.h>

namespace fcip::editor {

enum class Icon {
    Undo, Terrain, AddObject, EditObject, Environment,
    Raise, Lower, Bump, Dig, SetHeight, Extend, Smooth, Trim, Paint, Erase,
};

// in the square from min, size pixels across
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 min, float size, ImU32 colour);

}  // namespace fcip::editor
