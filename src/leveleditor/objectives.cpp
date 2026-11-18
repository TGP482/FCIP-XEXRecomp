// ige.xex, the solo level editor: the level's objectives. Each is a Waypoint entity (its script, lua.dat 0xa126a704):
// TableIndex orders it in the objectives list, selStatus is where it starts (Absent, Open, Current, Closed),
// WaypointTag and CompletionTag name its text in the objectives, Radius and bTriggeredOnEnter complete it when the
// player gets there. Edits go to the entity as the Properties tab's do, each one step of undo. The text the tags name
// is the mission's string table's (mission_text.cpp). The extraction point is the objective the level ends at once
// every other one is done; the save gives each objective its marker and the extraction its end trigger and watch
// (custom_save.cpp).
#include "leveleditor/objectives.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "leveleditor/fields.h"
#include "leveleditor/level_data.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission_text.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

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

// a tag's text in a section of the mission's string table
struct TagText {
    const char* key;
    const char* section;
    const char* label;
};
constexpr TagText kTexts[] = {
    {"WaypointTag", "Waypoints", "Objective text"},
    {"CompletionTag", "Completions", "Completion text"},
};

constexpr const char* kExtraction = "Extraction";  // LevelInfo: the extraction waypoint's name
constexpr const char* kNoExtraction = "None";

int IndexOf(const ViewEntity& e) { return std::atoi(PropertyOf(e, "TableIndex").c_str()); }

void ExtractionCombo(const std::vector<const ViewEntity*>& waypoints) {
    std::string chosen = LevelInfoValue(kExtraction);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Extraction point");
    ImGui::SameLine(ImGui::GetFontSize() * 12);
    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo("##extraction", chosen.empty() ? kNoExtraction : chosen.c_str())) return;
    if (ImGui::Selectable(kNoExtraction, chosen.empty())) SetLevelInfoValue(kExtraction, "");
    for (const ViewEntity* e : waypoints) {
        if (ImGui::Selectable(e->name.c_str(), chosen == e->name)) SetLevelInfoValue(kExtraction, e->name);
    }
    ImGui::EndCombo();
}

// the script reads selStatus; its enumStatus is the choice list itself
void StatusCombo(const ViewEntity& e) {
    ImGui::SetNextItemWidth(-1);
    std::string edited;
    if (DrawChoice("##status", ChoicesOf("selStatus"), PropertyOf(e, "selStatus"), edited)) {
        SetEntityProperty(e.id, "selStatus", edited);
    }
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
    for (const TagText& t : kTexts) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.label);
        ImGui::SameLine(label_w);
        std::string tag = PropertyOf(e, t.key);
        if (tag.empty() || !HasStringTable()) {
            ImGui::TextDisabled(tag.empty() ? "no tag" : "no string table loaded");
            continue;
        }
        ImGui::SetNextItemWidth(-1);
        std::string edited;
        if (DrawField(t.label, FieldKind::Text, MissionText(t.section, tag), edited)) {
            SetMissionText(t.section, tag, edited);
        }
    }
    ImGui::Separator();
    ImGui::PopID();
}

}  // namespace

void fcip::editor::DrawObjectivesPanel() {
    ImGui::TextUnformatted("OBJECTIVES");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Objective Waypoints, placed from the Objects tab's Gameplay");
    ImGui::TextUnformatted("Their text saves with the level and shows once it opens again");
    ImGui::PopStyleColor();
    ImGui::Separator();
    std::vector<const ViewEntity*> waypoints;
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.cls == "Waypoint" && !e.properties.empty()) waypoints.push_back(&e);
    }
    std::sort(waypoints.begin(), waypoints.end(), [](auto a, auto b) { return IndexOf(*a) < IndexOf(*b); });
    if (waypoints.empty()) {
        ImGui::TextUnformatted("This level has no objectives.");
    } else {
        ExtractionCombo(waypoints);
        ImGui::Separator();
    }
    ImGui::BeginChild("##objectives");
    for (const ViewEntity* e : waypoints) DrawObjective(*e);
    ImGui::EndChild();
}
