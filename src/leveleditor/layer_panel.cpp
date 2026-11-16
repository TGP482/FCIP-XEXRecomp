// the solo level editor's Layers tab: each layer shown or hidden, made, renamed or deleted, and the picked entity moved
// between them
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "leveleditor/layers.h"
#include "leveleditor/markers.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

std::string g_current;  // the layer Rename and Delete act on
char g_name[64] = {};

bool Has(const std::vector<std::string>& layers, const std::string& name) {
    return std::find(layers.begin(), layers.end(), name) != layers.end();
}

void DrawList(const std::vector<std::string>& layers) {
    if (ImGui::Button("Show All")) ShowAllLayers(true);
    ImGui::SameLine();
    if (ImGui::Button("Hide All")) ShowAllLayers(false);
    for (const std::string& layer : layers) {
        ImGui::PushID(layer.c_str());
        bool shown = LayerShown(layer);
        if (ImGui::Checkbox("##shown", &shown)) ShowLayer(layer, shown);
        ImGui::SameLine();
        if (ImGui::Selectable(layer.c_str(), g_current == layer)) {
            g_current = layer;
            std::snprintf(g_name, sizeof(g_name), "%s", layer.c_str());
        }
        ImGui::PopID();
    }
}

void DrawEdit(const std::vector<std::string>& layers) {
    ImGui::InputText("Name", g_name, sizeof(g_name));
    std::string name = g_name;
    bool valid = !name.empty() && name != kNoLayer;
    bool real = Has(layers, g_current) && g_current != kNoLayer;
    ImGui::BeginDisabled(!valid || Has(layers, name));
    if (ImGui::Button("Create")) {
        CreateLayer(name);
        g_current = name;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!real || !valid || name == g_current);
    if (ImGui::Button("Rename")) {
        RenameLayer(g_current, name);
        g_current = name;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!real);
    if (ImGui::Button("Delete")) {
        DeleteLayer(g_current);
        g_current.clear();
    }
    ImGui::EndDisabled();
}

std::string NameOf(uint32_t id) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.id == id) return e.name;
    }
    return {};
}

void DrawPicked(std::vector<std::string> layers) {
    uint32_t id = SelectedEntityId();
    if (!id) {
        ImGui::TextDisabled("Pick an entity to move it to another layer");
        return;
    }
    if (!Has(layers, kNoLayer)) layers.push_back(kNoLayer);
    std::string layer = LayerOf(id);
    if (layer.empty()) layer = kNoLayer;
    ImGui::TextUnformatted(NameOf(id).c_str());
    if (!ImGui::BeginCombo("Layer", layer.c_str())) return;
    for (const std::string& l : layers) {
        if (ImGui::Selectable(l.c_str(), l == layer) && l != layer) MoveToLayer(id, l);
    }
    ImGui::EndCombo();
}

}  // namespace

void fcip::editor::DrawLayerPanel() {
    ImGui::TextUnformatted("LAYERS");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("A hidden layer hides its markers and models while editing");
    ImGui::PopStyleColor();
    ImGui::Separator();
    std::vector<std::string> layers = ViewLayers();
    DrawList(layers);
    ImGui::SeparatorText("Layer");
    DrawEdit(layers);
    ImGui::SeparatorText("Picked Entity");
    DrawPicked(layers);
}
