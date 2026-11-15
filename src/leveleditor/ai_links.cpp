// ige.xex, the solo level editor: the AI's links, as the AI reads them (ai_markers.cpp draws them). An enemy or AI
// vehicle goes to the anchors its PropertiesInstance names (FirstAnchor, OnPlayerSeenAnchor, OnCoolDownAnchor) and a
// vehicle drives its pathname; a CreatureGeneratorSpawnSpot spawns for the CreatureGenerator its NameOfSpawnEntity
// names. An anchor leads to its NextAnchor, and its actions (Move, LookAt, OrientTo, ShootAt, Relocate, JumpTo)
// name their Target, SwitchToAnchor its Name.
#include "leveleditor/ai_links.h"

#include <iterator>
#include <memory>
#include <string>

#include <imgui.h>

#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

constexpr const char* kTargetActions[] = {"Move", "LookAt", "OrientTo", "ShootAt", "Relocate", "JumpTo"};

const ViewEntity* EntityOf(uint32_t id) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const AiObject* ObjectOf(uint32_t node) {
    for (const AiObject& o : ReadAiObjects()->objects) {
        if (o.node == node) return &o;
    }
    return nullptr;
}

// an anchor field (FirstAnchor1, OnPlayerSeenAnchor), not a group number (AnchorGroupID)
bool IsAnchorKey(const std::string& key) {
    return key.find("Anchor") != std::string::npos && key.find("ID") == std::string::npos;
}

// the entity's property key set to the target's name, or cleared when it already names it
void PropertyRow(const ViewEntity& e, const std::string& key, const Endpoint& to) {
    std::string value = PropertyOf(e, key.c_str());
    bool named = value == to.name;
    ImGui::PushID(key.c_str());
    if (ImGui::SmallButton(named ? "Clear" : "Set")) SetEntityProperty(e.id, key, named ? "" : to.name);
    ImGui::SameLine();
    ImGui::Text("%s: %s", key.c_str(), value.empty() ? "(none)" : value.c_str());
    ImGui::PopID();
}

bool DrawEntityToObject(const ViewEntity& e, const Endpoint& to) {
    bool any = false;
    for (const XmlAttribute& a : e.properties) {
        bool anchor = to.kind == "AIAnchor" && IsAnchorKey(a.key);
        bool path = to.kind == "AIPath" && a.key == "pathname";
        if (!anchor && !path) continue;
        PropertyRow(e, a.key, to);
        any = true;
    }
    return any;
}

std::string ElementAttr(const XmlElement& e, const char* key) {
    for (const XmlAttribute& a : e.attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

// the anchor's actions that go to the target, each removable
void DrawActionsTo(const AiObject& anchor, const std::string& name) {
    for (size_t i = 1; i < anchor.elements.size(); i++) {
        const XmlElement& e = anchor.elements[i];
        bool to = ElementAttr(e, "Target") == name || (e.tag == "SwitchToAnchor" && ElementAttr(e, "Name") == name);
        if (!to) continue;
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) RemoveAiChild(anchor.node, e.path);
        ImGui::SameLine();
        ImGui::Text("%s %s", e.tag.c_str(), name.c_str());
        ImGui::PopID();
    }
}

bool DrawAnchorTo(const AiObject& anchor, const Endpoint& to) {
    DrawActionsTo(anchor, to.name);
    if (to.kind == "AIAnchor") {
        bool next = AttributeOf(anchor, "NextAnchor") == to.name;
        if (ImGui::SmallButton(next ? "Clear NextAnchor" : "Make NextAnchor")) {
            SetAiAttribute(anchor.node, {}, "NextAnchor", next ? "" : to.name);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add SwitchToAnchor")) AddAiAction(anchor.node, "SwitchToAnchor", {{"Name", to.name}});
    }
    ImGui::TextUnformatted("Add an action to it:");
    for (const char* tag : kTargetActions) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tag)) AddAiAction(anchor.node, tag, {{"Target", to.name}});
    }
    return true;
}

}  // namespace

void fcip::editor::DrawAiLinks(const Endpoint& from, const Endpoint& to) {
    bool any = false;
    ImGui::SeparatorText("AI");
    const ViewEntity* entity = from.entity_id ? EntityOf(from.entity_id) : nullptr;
    if (entity && to.object) any = DrawEntityToObject(*entity, to);
    if (entity && from.kind == "CreatureGeneratorSpawnSpot" && to.kind == "CreatureGenerator") {
        PropertyRow(*entity, "NameOfSpawnEntity", to);
        any = true;
    }
    const AiObject* anchor = from.kind == "AIAnchor" ? ObjectOf(from.object) : nullptr;
    if (anchor) any = DrawAnchorTo(*anchor, to);
    if (!any) ImGui::TextUnformatted("No AI link joins these two.");
}
