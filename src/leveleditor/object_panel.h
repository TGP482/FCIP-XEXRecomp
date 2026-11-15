// the solo level editor's object properties and event links, drawn in the map editor's dock
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "leveleditor/event_links.h"
#include "leveleditor/mission.h"

struct PPCContext;

namespace fcip::editor {

void DrawObjectPanel();  // window thread, in the ImGui frame
uint32_t SelectedEntityId();  // any thread, the one entity the Edit Object tool has selected, 0 for none
// window thread: one of an entity's Properties, a sub table's as "Sub/key", set at the editor's next update with undo
void SetEntityProperty(uint32_t entity_id, const std::string& key, const std::string& value);
void SetEntityLinks(uint32_t entity_id, std::vector<Link> links);  // window thread, all its links, with undo
// editor thread: an entity's Properties then PropertiesInstance as the editor saves them, a sub table's keys as
// "Sub/key"
std::vector<XmlAttribute> ReadProperties(PPCContext& ctx, uint32_t entity);

}  // namespace fcip::editor
