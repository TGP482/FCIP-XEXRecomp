// a click in the editor view picks the marker under it: an entity into the Edit Object tool's selection, a mission
// object into the AI tab (picking.cpp)
#pragma once
#include <cstdint>

#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

enum DockTab : int { kNoTab, kPropertiesTab, kAiTab };

// window thread
bool PickMarker(float x, float y);  // display pixels; true when a marker took the click
DockTab TakeDockTab();              // the tab the last pick shows, once
void DrawPicked(const View& v);     // the picked mission object

// editor thread
void ApplyPick(PPCContext& ctx, uint32_t tool);  // the Edit Object tool's update (r3 the tool)
uint32_t EditorObjectOfEntity(PPCContext& ctx, uint32_t entity_id);  // 0 for none

}  // namespace fcip::editor
