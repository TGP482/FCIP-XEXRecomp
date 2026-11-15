// the solo level editor's object properties and event links, drawn in the map editor's dock
#pragma once
#include <cstdint>
#include <vector>

#include "leveleditor/mission.h"

struct PPCContext;

namespace fcip::editor {

void DrawObjectPanel();  // window thread, in the ImGui frame
// editor thread: an entity's Properties then PropertiesInstance as the editor saves them, a sub table's keys as
// "Sub/key"
std::vector<XmlAttribute> ReadProperties(PPCContext& ctx, uint32_t entity);

}  // namespace fcip::editor
