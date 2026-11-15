// the level editor's markers over the frame for the logic objects, which have no model
#pragma once

struct PPCContext;

namespace fcip::editor {

void UpdateMarkers(PPCContext& ctx);  // editor thread, each frame of the Add and Edit Object tools
void DrawMarkers();                   // window thread, in the ImGui frame

}  // namespace fcip::editor
