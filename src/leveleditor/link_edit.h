// links edited in the solo editor: Shift and a drag from the picked entity or mission object to another opens a window
// with what joins them; the Properties tab's event fields and mission calls share the lists (link_edit.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include "leveleditor/event_links.h"
#include "leveleditor/view_draw.h"

namespace fcip::editor {

// an entity or a mission object a link joins
struct Endpoint {
    uint32_t entity_id = 0;
    uint32_t object = 0;
    std::string name;
    std::string kind;  // its class or Type
    float pos[3] = {};
};

// window thread
bool LinkMouse(ImVec2 mouse, bool pressed, bool down);  // true while a link drag has the mouse
void DrawLinkDrag(const View& v);  // the line being dragged
void DrawLinkWindow();             // what joins the two a drag picked

std::string ClassOfEntity(uint32_t entity_id);  // empty for none
// a text field with the usual values in a list beside it
void EventField(const char* id, char* text, size_t bytes, const std::vector<std::string>& usual, float width);
// a mission call from the entity, added to its links on Add
void DrawMissionCallRow(uint32_t entity_id, const std::vector<Link>& links);

}  // namespace fcip::editor
