// ige.xex, the solo level editor: the AI tab lists the campaign level's AI objects. One picked shows its attributes
// and children (an anchor's actions, a shape's points) as a tree to edit; new tag points and anchors go at the tool
// cursor. Edits change the kept mission, which the save writes; the AI in the editor runs what the level loaded.
#include "leveleditor/ai_panel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"

using namespace fcip::editor;

namespace {

// the anchor actions ige's action factory knows (828C3FC4), and a shape's or path's point
constexpr const char* kChildTags[] = {
    "Action", "Bind", "Disable", "DoReadibility", "Enable", "IgnoreEvents", "JumpTo", "LookAt", "Move",
    "OrientTo", "PlayAnimation", "PlaySequence", "PlaySound", "Random", "Relocate", "Repeat", "Say", "SendSignal",
    "SetAlertnessLevel", "ShootAt", "SwitchToAnchor", "SwitchToBehavior", "Try", "Unbind", "Wait", "Point",
};
constexpr const char* kAllTypes = "All types";
constexpr size_t kValueChars = 256;
constexpr size_t kNameChars = 64;
constexpr float kListLines = 12.0f;

std::shared_ptr<const AiObjects> g_view;
uint32_t g_selected = 0;
int g_element = 0;  // the picked object's element the add rows act on
uint32_t g_fields_generation = 0;
uint32_t g_fields_object = 0;
std::vector<std::array<char, kValueChars>> g_fields;
std::string g_type = kAllTypes;
char g_filter[kNameChars] = "";
char g_new_key[kNameChars] = "";
char g_new_value[kValueChars] = "";
char g_new_tag[kNameChars] = "Move";

const AiObject* Picked() {
    for (const AiObject& o : g_view->objects) {
        if (o.node == g_selected) return &o;
    }
    return nullptr;
}

// the attribute fields follow the snapshot, so a value typed but not applied is lost when it changes
void SyncFields(const AiObject* o) {
    uint32_t object = o ? o->node : 0;
    if (g_fields_generation == g_view->generation && g_fields_object == object) return;
    g_fields_generation = g_view->generation;
    g_fields_object = object;
    g_fields.clear();
    if (!o) return;
    g_element = std::clamp(g_element, 0, int(o->elements.size()) - 1);
    for (const XmlElement& e : o->elements) {
        for (const XmlAttribute& a : e.attributes) {
            std::array<char, kValueChars> field{};
            a.value.copy(field.data(), kValueChars - 1);
            g_fields.push_back(field);
        }
    }
}

std::array<float, 3> Cursor() {
    State s = Read();
    return {s.cursor[0], s.cursor[1], s.cursor[2]};
}

bool Matches(const AiObject& o) {
    if (g_type != kAllTypes && o.type != g_type) return false;
    return !g_filter[0] || o.name.find(g_filter) != std::string::npos;
}

void DrawFilters() {
    std::set<std::string> types;
    for (const AiObject& o : g_view->objects) types.insert(o.type);
    float em = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::BeginCombo("##type", g_type.c_str())) {
        if (ImGui::Selectable(kAllTypes, g_type == kAllTypes)) g_type = kAllTypes;
        for (const std::string& t : types) {
            if (ImGui::Selectable(t.c_str(), g_type == t)) g_type = t;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "name", g_filter, kNameChars);
    if (ImGui::Button("Add Tag Point")) AddAiObject("TagPoint", Cursor());
    ImGui::SameLine();
    if (ImGui::Button("Add Anchor")) AddAiObject("AIAnchor", Cursor());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("at the tool cursor");
    ImGui::PopStyleColor();
}

void DrawList() {
    float height = ImGui::GetTextLineHeightWithSpacing() * kListLines;
    ImGui::BeginChild("##ai_list", ImVec2(0, height), ImGuiChildFlags_Border);
    for (const AiObject& o : g_view->objects) {
        if (!Matches(o)) continue;
        std::string label = o.name + " (" + o.type + ")##" + std::to_string(o.node);
        if (ImGui::Selectable(label.c_str(), o.node == g_selected)) {
            g_selected = o.node;
            g_element = 0;
        }
    }
    ImGui::EndChild();
}

void DrawElementButtons(const AiObject& o, const XmlElement& e) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Up")) MoveAiChild(o.node, e.path, -1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Down")) MoveAiChild(o.node, e.path, 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) RemoveAiChild(o.node, e.path);
}

void DrawElements(const AiObject& o) {
    float em = ImGui::GetFontSize();
    float label_w = em * 9;
    size_t field = 0;
    for (size_t i = 0; i < o.elements.size(); i++) {
        const XmlElement& e = o.elements[i];
        float indent = em * float(e.depth);
        ImGui::PushID(int(i));
        if (indent > 0) ImGui::Indent(indent);
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
        if (ImGui::Selectable(e.tag.c_str(), int(i) == g_element, 0, ImVec2(em * 10, 0))) g_element = int(i);
        ImGui::PopStyleColor();
        if (e.depth > 0) DrawElementButtons(o, e);
        for (const XmlAttribute& a : e.attributes) {
            ImGui::PushID(int(field));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.key.c_str());
            ImGui::SameLine(indent + label_w);
            ImGui::SetNextItemWidth(-1);
            char* text = g_fields[field].data();
            ImGui::InputText("##value", text, kValueChars);
            if (ImGui::IsItemDeactivatedAfterEdit()) SetAiAttribute(o.node, e.path, a.key, text);
            ImGui::PopID();
            field++;
        }
        if (indent > 0) ImGui::Unindent(indent);
        ImGui::PopID();
    }
}

void DrawTagField(float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##tag", g_new_tag, kNameChars);
    ImGui::SameLine(0, 0);
    if (!ImGui::BeginCombo("##tags", nullptr, ImGuiComboFlags_NoPreview)) return;
    for (const char* t : kChildTags) {
        if (ImGui::Selectable(t)) std::snprintf(g_new_tag, kNameChars, "%s", t);
    }
    ImGui::EndCombo();
}

// the add rows act on the element picked in the tree
void DrawAddRows(const AiObject& o) {
    const XmlElement& e = o.elements[size_t(g_element)];
    float em = ImGui::GetFontSize();
    ImGui::Separator();
    ImGui::Text("In %s:", e.tag.c_str());
    ImGui::SetNextItemWidth(em * 8);
    ImGui::InputTextWithHint("##key", "attribute", g_new_key, kNameChars);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10);
    ImGui::InputTextWithHint("##value", "value", g_new_value, kValueChars);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_new_key[0]);
    if (ImGui::Button("Set")) SetAiAttribute(o.node, e.path, g_new_key, g_new_value);
    ImGui::EndDisabled();
    DrawTagField(em * 8);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_new_tag[0]);
    if (ImGui::Button("Add Child")) AddAiChild(o.node, e.path, g_new_tag);
    ImGui::EndDisabled();
}

bool IsArea(const AiObject& o) { return o.type == "Shape" || o.type == "AreaBox" || o.type == "AreaSphere"; }

// the entities an area tells when something enters it (OnEnterArea), picked from the level's
void DrawAreaEntities(const AiObject& o) {
    ImGui::Separator();
    ImGui::TextUnformatted("Tells:");
    for (size_t i = 0; i < o.entities.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) {
            std::vector<std::string> names = o.entities;
            names.erase(names.begin() + std::ptrdiff_t(i));
            SetAiEntities(o.node, names);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(o.entities[i].c_str());
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo("##tell", "Add an entity")) return;
    std::set<std::string> names;
    for (const ViewEntity& e : *ViewEntities()) {
        if (!e.name.empty()) names.insert(e.name);
    }
    for (const std::string& name : names) {
        if (!ImGui::Selectable(name.c_str())) continue;
        std::vector<std::string> told = o.entities;
        told.push_back(name);
        SetAiEntities(o.node, told);
    }
    ImGui::EndCombo();
}

// the object and its points to the cursor
void MoveToCursor(const AiObject& o) {
    std::array<float, 3> to = Cursor();
    PlaceAiObject(o.node, {to[0] - o.pos[0], to[1] - o.pos[1], to[2] - o.pos[2]}, {});
}

void DrawPicked(const AiObject& o) {
    ImGui::Separator();
    ImGui::Text("%s (%s)", o.name.c_str(), o.type.c_str());
    if (o.placed) {
        if (ImGui::Button("Move to Cursor")) MoveToCursor(o);
        ImGui::SameLine();
    }
    if (ImGui::Button("Delete")) RemoveAiObject(o.node);
    if (IsArea(o)) DrawAreaEntities(o);
    DrawAddRows(o);
    ImGui::Separator();
    ImGui::BeginChild("##ai_elements");
    DrawElements(o);
    ImGui::EndChild();
}

}  // namespace

namespace fcip::editor {

uint32_t SelectedAiObject() { return g_selected; }

void SelectAiObject(uint32_t node) {
    if (node == g_selected) return;
    g_selected = node;
    g_element = 0;
}

bool DeleteSelectedAiObject() {
    if (!g_selected) return false;
    RemoveAiObject(g_selected);
    g_selected = 0;
    return true;
}

void DrawAiPanel() {
    g_view = ReadAiObjects();
    ImGui::TextUnformatted("AI OBJECTS");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Kept for the level save; the AI in the editor runs what the level loaded");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (g_view->objects.empty()) {
        ImGui::TextUnformatted("Open a campaign level to edit its AI objects.");
        return;
    }
    DrawFilters();
    DrawList();
    const AiObject* picked = Picked();
    if (!picked) g_selected = 0;
    SyncFields(picked);
    if (picked) DrawPicked(*picked);
}

}  // namespace fcip::editor
