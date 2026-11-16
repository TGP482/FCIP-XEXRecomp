// the solo editor's value widgets: a check box for a flag, drags for numbers and vectors, a colour picker for a colour
// (the level files keep colours as "r,g,b" from 0 to 1), text for the rest. The widget being edited keeps its own
// value until the edit ends, then the panel queues one change, so a drag is one step of undo. A selX property picks
// from its script's enumX list by index; file, model, texture and effect names pick from what the campaign names.
#include "leveleditor/fields.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <imgui.h>

#include "leveleditor/property_lists.h"

namespace {

constexpr size_t kTextChars = 256;
constexpr float kFloatSpeed = 0.01f;

ImGuiID g_editing = 0;
std::string g_text;  // the edited value while g_editing
char g_filter[kTextChars];  // the open picker's filter

std::string Format(const float* v, int count) {
    std::string text;
    for (int i = 0; i < count; i++) {
        char part[32];
        std::snprintf(part, sizeof(part), "%s%g", i ? "," : "", v[i]);
        text += part;
    }
    return text;
}

// up to count comma separated numbers, the rest 0
void Parse(const std::string& text, float* v, int count) {
    const char* at = text.c_str();
    for (int i = 0; i < count; i++) {
        char* end = nullptr;
        v[i] = std::strtof(at, &end);
        at = *end == ',' ? end + 1 : end;
    }
}

// a colour the mission keeps as "r,g,b" from 0 to 255
bool Colour255(std::string& text) {
    float v[3];
    Parse(text, v, 3);
    for (float& c : v) c /= 255.0f;
    if (!ImGui::ColorEdit3("##v", v)) return false;
    char out[32];
    std::snprintf(out, sizeof(out), "%d,%d,%d", int(v[0] * 255.0f + 0.5f), int(v[1] * 255.0f + 0.5f),
                  int(v[2] * 255.0f + 0.5f));
    text = out;
    return true;
}

// the widget for the kind; true when it changed text
bool Widget(fcip::editor::FieldKind kind, std::string& text) {
    using fcip::editor::FieldKind;
    float v[3] = {};
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
        case FieldKind::Colour255:
            return Colour255(text);
        case FieldKind::Vector:
            Parse(text, v, 3);
            if (!ImGui::DragFloat3("##v", v, kFloatSpeed, 0.0f, 0.0f, "%g")) return false;
            text = Format(v, 3);
            return true;
        case FieldKind::Vector2:
            Parse(text, v, 2);
            if (!ImGui::DragFloat2("##v", v, kFloatSpeed, 0.0f, 0.0f, "%g")) return false;
            text = Format(v, 2);
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

// a script property's type prefix: lower case, then a capital or an underscore (selStatus, texture_CoronaTexture)
bool HasPrefix(const std::string& key, const char* prefix) {
    size_t n = std::strlen(prefix);
    return key.size() > n && key.compare(0, n, prefix) == 0 && (std::isupper(uint8_t(key[n])) || key[n] == '_');
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::tolower(uint8_t(c)));
    return out;
}

// a path's extension in lower case, empty for none
std::string ExtensionOf(const std::string& path) {
    size_t dot = path.rfind('.');
    return dot == std::string::npos ? std::string() : Lower(std::string_view(path).substr(dot + 1));
}

}  // namespace

fcip::editor::FieldKind fcip::editor::KindOfKey(const std::string& key) {
    if (key.rfind("clr", 0) == 0) return FieldKind::Colour;
    if (key.rfind("vector", 0) == 0) return FieldKind::Vector;
    if (HasPrefix(key, "b")) return FieldKind::Bool;
    if (HasPrefix(key, "f")) return FieldKind::Float;
    if (HasPrefix(key, "i") || HasPrefix(key, "n")) return FieldKind::Int;
    return FieldKind::Text;
}

bool fcip::editor::DrawPropertyValue(const char* id, const std::string& key, const std::string& value,
                                     std::string& out) {
    std::vector<std::string> choices = ChoicesOf(key);
    if (!choices.empty()) return DrawChoice(id, choices, value, out);
    std::span<const char* const> items = PickerItemsOf(key, value);
    if (!items.empty()) return DrawPicker(id, items, value, out);
    return DrawField(id, KindOfKey(key), value, out);
}

std::vector<std::string> fcip::editor::ChoicesOf(const std::string& key) {
    std::vector<std::string> names;
    if (!HasPrefix(key, "sel")) return names;
    for (const ChoiceList& list : kChoiceLists) {
        if (key.compare(3, std::string::npos, list.name) != 0) continue;
        std::string_view all = list.choices;
        for (size_t bar; (bar = all.find('|')) != std::string_view::npos; all.remove_prefix(bar + 1)) {
            names.emplace_back(all.substr(0, bar));
        }
        names.emplace_back(all);
    }
    return names;
}

std::span<const char* const> fcip::editor::PickerItemsOf(const std::string& key, const std::string& value) {
    if (HasPrefix(key, "effect")) return kEffects;
    bool model = HasPrefix(key, "file") || HasPrefix(key, "obj") || HasPrefix(key, "object");
    bool texture = HasPrefix(key, "tex") || HasPrefix(key, "texture");
    bool video = HasPrefix(key, "bink");
    if (!model && !texture && !video) return {};
    std::string ext = ExtensionOf(value);
    if (ext == "xbt") return kTextures;
    if (ext == "xba") return kAnimations;
    if (ext == "bik") return kVideos;
    if (ext == "xbg" || ext == "glm" || ext == "cgf") return kModels;
    if (texture) return kTextures;
    if (video) return kVideos;
    // empty: fileAnimPackage and fileRopeAnim name animations, objAnimObject a model
    bool anim = key.find("Anim") != std::string::npos && !key.ends_with("Object");
    return anim ? std::span<const char* const>(kAnimations) : std::span<const char* const>(kModels);
}

bool fcip::editor::DrawChoice(const char* id, const std::vector<std::string>& names, const std::string& value,
                              std::string& out) {
    int index = std::atoi(value.c_str());
    bool known = index >= 0 && index < int(names.size());
    std::string preview = known ? names[size_t(index)] : value;
    bool picked = false;
    ImGui::PushID(id);
    if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (int i = 0; i < int(names.size()); i++) {
            if (!ImGui::Selectable(names[size_t(i)].c_str(), i == index)) continue;
            out = std::to_string(i);
            picked = true;
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return picked;
}

bool fcip::editor::DrawPicker(const char* id, std::span<const char* const> items, const std::string& value,
                              std::string& out) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight());
    bool done = DrawField("##text", FieldKind::Text, value, out);
    ImGui::SameLine(0, 0);
    if (ImGui::BeginCombo("##list", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            g_filter[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "filter", g_filter, sizeof(g_filter));
        std::string filter = Lower(g_filter);
        std::string current = Lower(value);
        for (const char* item : items) {
            std::string lower = Lower(item);
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
            if (!ImGui::Selectable(item, lower == current)) continue;
            out = item;
            done = true;
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return done;
}

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
