// the solo level editor's Static Geometry tab: the terrain sectors' brushes shown, picked and placed
// (static_geometry.cpp)
#pragma once

#include <imgui.h>

#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

// window thread
void DrawStaticGeometryPanel();
void DrawStaticGeometry(const View& v);        // the picked brush and, when shown, the boxes near the camera
bool BrushMouse(ImVec2 mouse, bool pressed);  // true when picking took the click

void UpdateStaticGeometry(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh read after any

}  // namespace fcip::editor
