// the gizmo on what the Edit Object tool has picked in the solo editor: arrows move it along x, y and z, rings turn
// it about them (gizmo.cpp)
#pragma once
#include <imgui.h>

#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

// window thread
bool GizmoMouse(ImVec2 mouse, bool pressed, bool down);  // true while the gizmo has the mouse
void DrawGizmo(const View& v);

void ApplyGizmo(PPCContext& ctx);  // editor thread, the Edit Object tool's update

}  // namespace fcip::editor
