// the kept mission's XML nodes read and edited from the editor thread, for the solo editor's tabs (node_edit.cpp)
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "leveleditor/mission.h"

struct PPCContext;

namespace fcip::editor {

// any thread with the node kept: its attributes in order, one of them (empty when missing), a child by tag (0 for
// none)
std::vector<XmlAttribute> XmlAttributes(uint32_t node);
std::string XmlValue(uint32_t node, const std::string& key);
uint32_t XmlChild(uint32_t node, const std::string& tag);

// editor thread
void SetXmlValue(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value);
uint32_t AddXmlChild(PPCContext& ctx, uint32_t parent, const std::string& tag);  // parent keeps it
void RemoveXmlChild(PPCContext& ctx, uint32_t parent, uint32_t child);
// the mission's child tagged tag changed by change, then reload; one step of undo that puts a copy of it as it was
// back and reloads again
using NodeChange = std::function<void(PPCContext&, uint32_t)>;
void EditMissionNode(PPCContext& ctx, const std::string& tag, const std::string& label, const NodeChange& change,
                     const std::function<void(PPCContext&)>& reload);

}  // namespace fcip::editor
