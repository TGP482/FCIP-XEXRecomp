// ige.xex, the solo level editor: the level's objectives. Each is a Waypoint entity (its script, lua.dat 0xa126a704):
// TableIndex orders it in the objectives list, selStatus is where it starts (Absent, Open, Current, Closed),
// WaypointTag and CompletionTag name its text in the objectives, Radius and bTriggeredOnEnter complete it when the
// player gets there. Edits go to the entity as the Properties tab's do, each one step of undo.
#include "leveleditor/objectives.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "leveleditor/fields.h"
#include "leveleditor/markers.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

constexpr const char* kStatuses[] = {"Absent", "Open", "Current", "Closed"};
struct Column {
    const char* key;
    FieldKind kind;
};
constexpr Column kColumns[] = {
    {"TableIndex", FieldKind::Int},        {"WaypointTag", FieldKind::Text},
    {"CompletionTag", FieldKind::Text},    {"Radius", FieldKind::Float},
    {"bTriggeredOnEnter", FieldKind::Bool}, {"bDisplayNewObjectiveMessage", FieldKind::Bool},
    {"bBlinking", FieldKind::Bool},
};

int IndexOf(const ViewEntity& e) { return std::atoi(PropertyOf(e, "TableIndex").c_str()); }

// the editor keeps a list's choice twice, the script reads selStatus
void StatusCombo(const ViewEntity& e) {
    int status = std::clamp(std::atoi(PropertyOf(e, "selStatus").c_str()), 0, int(std::size(kStatuses)) - 1);
    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo("##status", kStatuses[status])) return;
    for (int i = 0; i < int(std::size(kStatuses)); i++) {
        if (!ImGui::Selectable(kStatuses[i], i == status)) continue;
        SetEntityProperty(e.id, "selStatus", std::to_string(i));
        SetEntityProperty(e.id, "enumStatus", std::to_string(i));
    }
    ImGui::EndCombo();
}

void DrawObjective(const ViewEntity& e) {
    ImGui::PushID(int(e.id));
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
    ImGui::TextUnformatted(e.name.c_str());
    ImGui::PopStyleColor();
    float label_w = ImGui::GetFontSize() * 12;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("selStatus");
    ImGui::SameLine(label_w);
    StatusCombo(e);
    for (const Column& c : kColumns) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(c.key);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        std::string edited;
        if (DrawField(c.key, c.kind, PropertyOf(e, c.key), edited)) SetEntityProperty(e.id, c.key, edited);
    }
    ImGui::Separator();
    ImGui::PopID();
}

}  // namespace

void fcip::editor::DrawObjectivesPanel() {
    ImGui::TextUnformatted("OBJECTIVES");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Objective Waypoints, placed from the Objects tab's Gameplay");
    ImGui::PopStyleColor();
    ImGui::Separator();
    std::vector<const ViewEntity*> waypoints;
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.cls == "Waypoint" && !e.properties.empty()) waypoints.push_back(&e);
    }
    std::sort(waypoints.begin(), waypoints.end(), [](auto a, auto b) { return IndexOf(*a) < IndexOf(*b); });
    if (waypoints.empty()) ImGui::TextUnformatted("This level has no objectives.");
    ImGui::BeginChild("##objectives");
    for (const ViewEntity* e : waypoints) DrawObjective(*e);
    ImGui::EndChild();
}
