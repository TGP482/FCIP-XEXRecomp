// the solo editor's value widgets: a check box for a flag, drags for numbers and vectors, a colour picker for a colour
// (the level files keep colours as "r,g,b" from 0 to 1), text for the rest. The widget being edited keeps its own
// value until the edit ends, then the panel queues one change, so a drag is one step of undo.
#include "leveleditor/fields.h"

#include <cstdio>
#include <cstdlib>

#include <imgui.h>

namespace {

constexpr size_t kTextChars = 256;
constexpr float kFloatSpeed = 0.01f;

ImGuiID g_editing = 0;
std::string g_text;  // the edited value while g_editing

std::string Format(const float* v, int count) {
    std::string text;
    for (int i = 0; i < count; i++) {
        char part[32];
        std::snprintf(part, sizeof(part), "%s%g", i ? "," : "", v[i]);
        text += part;
    }
    return text;
}

void Parse(const std::string& text, float* v, int count) {
    for (int i = 0; i < count; i++) v[i] = 0.0f;
    if (count == 1) v[0] = std::strtof(text.c_str(), nullptr);
    else std::sscanf(text.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
}

// the widget for the kind; true when it changed text
bool Widget(fcip::editor::FieldKind kind, std::string& text) {
    using fcip::editor::FieldKind;
    float v[3];
    switch (kind) {
        case FieldKind::Bool: {
            bool on = std::atoi(text.c_str()) != 0;
            if (!ImGui::Checkbox("##v", &on)) return false;
            text = on ? "1" : "0";
            return true;
        }
        case FieldKind::Int: {
            int i = std::atoi(text.c_str());
            if (!ImGui::DragInt("##v", &i)) return false;
            text = std::to_string(i);
            return true;
        }
        case FieldKind::Float:
            Parse(text, v, 1);
            if (!ImGui::DragFloat("##v", v, kFloatSpeed, 0.0f, 0.0f, "%g")) return false;
            text = Format(v, 1);
            return true;
        case FieldKind::Colour:
            Parse(text, v, 3);
            if (!ImGui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float)) return false;
            text = Format(v, 3);
            return true;
        case FieldKind::Vector:
            Parse(text, v, 3);
            if (!ImGui::DragFloat3("##v", v, kFloatSpeed, 0.0f, 0.0f, "%g")) return false;
            text = Format(v, 3);
            return true;
        case FieldKind::Text: {
            char buffer[kTextChars];
            std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
            if (!ImGui::InputText("##v", buffer, sizeof(buffer))) return false;
            text = buffer;
            return true;
        }
    }
    return false;
}

}  // namespace

bool fcip::editor::DrawField(const char* id, FieldKind kind, const std::string& value, std::string& out) {
    ImGui::PushID(id);
    ImGuiID me = ImGui::GetID("##v");
    std::string text = me == g_editing ? g_text : value;
    bool changed = Widget(kind, text);
    bool active = ImGui::IsItemActive();
    // a check box's click is its whole edit
    bool done = (kind == FieldKind::Bool && changed) || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    if (active) {
        g_editing = me;
        g_text = text;
    } else if (g_editing == me) {
        g_editing = 0;
    }
    if (done) out = text;
    return done;
}
