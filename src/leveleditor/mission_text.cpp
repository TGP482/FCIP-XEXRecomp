// ige.xex, the solo level editor: the level's mission script and its objectives' text. A campaign mission script
// loads its level's string table, Language:LoadMissionStringTable("01_Beach.xml") (8222B3C8) from
// LANGUAGES/<language>/, and a Waypoint shows the text its WaypointTag and CompletionTag name in the table's Waypoints
// and Completions sections. A custom level keeps its own copy of the table in its folder, which both executables read
// in place of the campaign's (campaign_levels.cpp, game/custom_level_start.cpp): text edited here goes into that copy
// when the level saves, and shows once the level opens again. The Script tab edits the custom level's Mission.lua,
// which runs again as soon as it is saved.
#include "leveleditor/mission_text.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>

#include <imgui.h>

#include "common/panel_colors.h"
#include "leveleditor/campaign_levels.h"

using namespace fcip::editor;

namespace {

constexpr const char* kSectionStart = "<section name=\"";
constexpr const char* kSectionEnd = "</section>";
constexpr const char* kTableEnd = "</stringtable>";
constexpr const char* kString = "<string";

using Key = std::pair<std::string, std::string>;  // section, tag

std::mutex g_lock;
std::string g_name;  // the table's file name, empty before a mission loads one
std::string g_text;  // as read
bool g_own = false;
std::map<Key, std::string> g_values;
std::map<Key, std::string> g_edits;

// window thread: the script as edited, and the file it came from
std::string g_script;
std::filesystem::path g_script_path;

std::string Attribute(const std::string& tag, const std::string& key) {
    std::string start = " " + key + "=\"";
    size_t at = tag.find(start);
    if (at == std::string::npos) return {};
    at += start.size();
    return tag.substr(at, tag.find('"', at) - at);
}

std::string Unescaped(std::string s) {
    constexpr std::pair<const char*, char> kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (auto [entity, c] : kEntities) {
        for (size_t at = s.find(entity); at != std::string::npos; at = s.find(entity, at + 1)) {
            s.replace(at, std::strlen(entity), 1, c);
        }
    }
    return s;
}

std::string Escaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::map<Key, std::string> Parse(const std::string& text) {
    std::map<Key, std::string> values;
    for (size_t s = text.find(kSectionStart); s != std::string::npos; s = text.find(kSectionStart, s + 1)) {
        std::string section = Attribute(text.substr(s, text.find('>', s) - s), "name");
        size_t end = text.find(kSectionEnd, s);
        for (size_t t = text.find(kString, s); t < end; t = text.find(kString, t + 1)) {
            std::string tag = text.substr(t, text.find('>', t) - t);
            values[{section, Attribute(tag, "enum")}] = Unescaped(Attribute(tag, "value"));
        }
    }
    return values;
}

// the table's text with a tag's value set, its string or section added when missing
void Patch(std::string& text, const Key& key, const std::string& value) {
    std::string entry = "<string enum=\"" + key.second + "\" value=\"" + Escaped(value) + "\"/>";
    size_t s = text.find(kSectionStart + key.first + "\"");
    if (s == std::string::npos) {
        size_t end = text.rfind(kTableEnd);
        if (end == std::string::npos) return;
        text.insert(end, "\t" + (kSectionStart + key.first) + "\">\r\n\t\t" + entry + "\r\n\t" + kSectionEnd + "\r\n");
        return;
    }
    size_t end = text.find(kSectionEnd, s);
    size_t t = text.find(" enum=\"" + key.second + "\"", s);
    if (t >= end) {
        text.insert(text.rfind('\n', end) + 1, "\t\t" + entry + "\r\n");
        return;
    }
    size_t close = text.find('>', t);
    size_t v = text.find(" value=\"", t);
    if (v < close) {
        v += std::strlen(" value=\"");
        text.replace(v, text.find('"', v) - v, Escaped(value));
    } else {
        text.insert(text[close - 1] == '/' ? close - 1 : close, " value=\"" + Escaped(value) + "\"");
    }
}

// a std::string's InputText buffer grows with it
int Resize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(size_t(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void LoadScript(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    g_script.assign(std::istreambuf_iterator<char>(in), {});
    g_script_path = path;
}

}  // namespace

void fcip::editor::KeepStringTable(const std::string& name, const std::string& text, bool own) {
    std::lock_guard lock(g_lock);
    g_name = name;
    g_text = text;
    g_own = own;
    g_values = Parse(text);
    g_edits.clear();
}

bool fcip::editor::HasStringTable() {
    std::lock_guard lock(g_lock);
    return !g_name.empty();
}

std::string fcip::editor::MissionText(const std::string& section, const std::string& tag) {
    std::lock_guard lock(g_lock);
    auto it = g_values.find({section, tag});
    return it == g_values.end() ? std::string() : it->second;
}

void fcip::editor::SetMissionText(const std::string& section, const std::string& tag, const std::string& text) {
    std::lock_guard lock(g_lock);
    g_values[{section, tag}] = text;
    g_edits[{section, tag}] = text;
}

StringTableFile fcip::editor::SavedStringTable() {
    std::lock_guard lock(g_lock);
    if (g_name.empty() || (!g_own && g_edits.empty())) return {};
    StringTableFile file{g_name, g_text};
    for (const auto& [key, value] : g_edits) Patch(file.text, key, value);
    return file;
}

void fcip::editor::DrawScriptPanel() {
    ImGui::TextUnformatted("MISSION SCRIPT");
    std::filesystem::path path = OwnScriptPath();
    std::error_code ec;
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    if (!std::filesystem::exists(path, ec)) {
        ImGui::TextWrapped("A custom level's Mission.lua, made when a campaign level first saves as one.");
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextWrapped("%s runs after the campaign's mission script in the custom level, and again each time it is "
                       "saved.", path.string().c_str());
    ImGui::PopStyleColor();
    if (path != g_script_path) LoadScript(path);
    if (ImGui::Button("Save")) std::ofstream(path, std::ios::binary) << g_script;
    ImGui::SameLine();
    if (ImGui::Button("Revert")) LoadScript(path);
    ImGui::Separator();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;
    ImGui::InputTextMultiline("##script", g_script.data(), g_script.capacity() + 1, ImVec2(-1, -1), flags, Resize,
                              &g_script);
}
