// handles on the picked volume in the solo editor: a trigger's or checkpoint's box sides, an AreaBox's sides and
// height, an AreaSphere's radius, an area's points and height (volumes.cpp)
#pragma once
#include <imgui.h>

#include "leveleditor/view_draw.h"

namespace fcip::editor {

// window thread
bool VolumeMouse(ImVec2 mouse, bool pressed, bool down);  // true while a handle has the mouse
void DrawVolumeHandles(const View& v);

}  // namespace fcip::editor
