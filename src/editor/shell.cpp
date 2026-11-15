// ige.xex: once a map is open FCIPEditor.exe is a desktop editor: a menu bar and toolbar over the 3D view, a dock on
// the right and a status bar, the map in the window's title. The frame is drawn into the view alone (SetViewBox), at
// the view's shape.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/panel_colors.h"
#include "common/view_clip.h"
#include "editor/icons.h"
#include "editor/tools.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/markers.h"
#include "leveleditor/mode.h"
#include "leveleditor/object_panel.h"

using namespace fcip;
using namespace fcip::editor;

namespace {

constexpr float kDockEms = 30.0f;  // the dock's first width
constexpr float kMinDockEms = 16.0f;
constexpr float kMinViewEms = 20.0f;
constexpr float kSplitterEms = 0.4f;
constexpr float kIconEms = 2.4f;
constexpr float kLabelPadEms = 0.5f;  // beside each toolbar label
constexpr float kParametersShare = 0.45f;  // of the dock's height, above the tabs

constexpr uint32_t kSetTitle = WM_APP + 3;  // the window thread sets g_title
constexpr const wchar_t* kGameName = L"Far Cry Instincts: Predator";

// a line of the controls window: what, then the keys
constexpr std::pair<const char*, const char*> kControls[] = {
    {"Look", "Right mouse drag"},     {"Move", "W A S D"},           {"Zoom", "Mouse wheel"},
    {"Use the tool", "Left click"},   {"Pick a tool", "1 to 4"},     {"Undo", "Ctrl+Z"},
    {"Delete object", "Del"},         {"Save", "Ctrl+S"},            {"Play test", "F5"},
    {"Settings", "F10"},
};

constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNav;

constexpr std::pair<ImGuiCol, ImVec4> kShellColors[] = {
    {ImGuiCol_MenuBarBg, kTab}, {ImGuiCol_PopupBg, kPanelBg}, {ImGuiCol_CheckMark, kText},
    {ImGuiCol_SliderGrab, kHeading}, {ImGuiCol_SliderGrabActive, kButtonHovered}, {ImGuiCol_Tab, kTab},
    {ImGuiCol_TabHovered, kTabHovered}, {ImGuiCol_TabActive, kHighlight},
};

struct ToolEntry {
    int tool;
    Icon icon;
    const char* key;
};
constexpr ToolEntry kToolEntries[] = {
    {kTerrain, Icon::Terrain, "1"}, {kAddObject, Icon::AddObject, "2"},
    {kEditObject, Icon::EditObject, "3"}, {kEnvironment, Icon::Environment, "4"},
};
constexpr Icon kBrushIcons[] = {Icon::Raise,     Icon::Lower,  Icon::Bump,   Icon::Dig,  Icon::SetHeight,
                                Icon::Extend,    Icon::Smooth, Icon::Trim,   Icon::Paint, Icon::Erase};
static_assert(std::size(kBrushIcons) == kBrushCount);

// window thread
float g_dock_width = 0.0f;  // pixels, 0 until the first frame
bool g_show_toolbar = true;
bool g_show_dock = true;
bool g_show_status = true;
bool g_show_markers = true;
bool g_show_controls = false;
char g_search[64] = "";
bool g_looking = false;   // the right button went down on the view
bool g_applying = false;  // the left button went down on the view

// Menus and toolbar

// the campaign's levels, opened as the game loads them, and the custom levels in levels\ beside the exe
void DrawOpenLevel() {
    if (ImGui::BeginMenu("Open Campaign Level")) {
        for (int i = 0; i < int(std::size(kCampaignLevels)); i++) {
            if (ImGui::MenuItem(kCampaignLevels[i])) OpenCampaignLevel(i);
        }
        ImGui::EndMenu();
    }
    std::vector<fcip::CustomLevel> custom = fcip::FindCustomLevels();
    if (!ImGui::BeginMenu("Open Custom Level", !custom.empty())) return;
    for (const fcip::CustomLevel& level : custom) {
        if (ImGui::MenuItem(level.name.c_str(), level.campaign.c_str())) OpenCustomLevel(level);
    }
    ImGui::EndMenu();
}

// OPEN A LEVEL's list over the level manager; true while it takes the mouse
bool DrawLevelList() {
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    float em = ImGui::GetFontSize();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 20.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Open a Level", &open, kFlags)) {
        ImGui::SeparatorText("Campaign Levels");
        for (int i = 0; i < int(std::size(kCampaignLevels)); i++) {
            if (ImGui::Selectable(kCampaignLevels[i])) OpenCampaignLevel(i);
        }
        ImGui::SeparatorText("Custom Levels");
        for (const fcip::CustomLevel& level : fcip::FindCustomLevels()) {
            if (ImGui::Selectable(level.name.c_str())) OpenCustomLevel(level);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
    if (!open) CloseLevelList();
    return true;
}

void DrawMenuBar(const State& s) {
    if (!ImGui::BeginMainMenuBar()) return;
    // new and open are the map manager's, the editor asks to save before it goes there
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(editor::g_solo ? "New Level..." : "New Map...")) ExitToMapManager();
        if (ImGui::MenuItem(editor::g_solo ? "Open Saved Level..." : "Open Map...")) ExitToMapManager();
        if (editor::g_solo) DrawOpenLevel();
        if (ImGui::MenuItem("Save", "Ctrl+S")) Save();
        ImGui::Separator();
        if (ImGui::MenuItem(editor::g_solo ? "Exit to Level Manager" : "Exit to Map Manager")) ExitToMapManager();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, s.undo_count > 0)) Undo(1);
        if (ImGui::MenuItem("Delete", "Del", false, s.tool == kEditObject)) DeleteSelected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Toolbar", nullptr, &g_show_toolbar);
        ImGui::MenuItem("Dock", nullptr, &g_show_dock);
        ImGui::MenuItem("Status Bar", nullptr, &g_show_status);
        ImGui::Separator();
        for (int option : {kOptionObjectIcons, kOptionOcean, kOptionFog}) {
            if (ImGui::MenuItem(OptionName(option), nullptr, s.options[option])) SetOption(option, !s.options[option]);
        }
        if (editor::g_solo) ImGui::MenuItem("Markers", nullptr, &g_show_markers);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Game")) {
        if (ImGui::MenuItem("Play Test", "F5")) PlayTest();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        for (const ToolEntry& t : kToolEntries) {
            if (ImGui::MenuItem(ToolName(t.tool), t.key, s.tool == t.tool)) SelectTool(t.tool);
        }
        ImGui::Separator();
        for (int b = 0; b < kBrushCount; b++) {
            if (ImGui::MenuItem(BrushName(b), nullptr, s.tool == kTerrain && s.brush == b)) SelectBrush(b);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Controls", nullptr, &g_show_controls);
        if (ImGui::MenuItem("Controller Layout")) ShowControllerLayout();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// the icon with its label beneath
bool IconButton(const char* id, Icon icon, const char* label, const char* tip, bool active, bool enabled) {
    float size = ImGui::GetFontSize() * kIconEms;
    ImVec2 text = ImGui::CalcTextSize(label);
    float width = std::fmax(size, text.x + ImGui::GetFontSize() * kLabelPadEms);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(width, size + text.y));
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (active) draw->AddRectFilled(min, max, ImGui::GetColorU32(kHighlight));
    else if (ImGui::IsItemHovered()) draw->AddRectFilled(min, max, ImGui::GetColorU32(kTabHovered));
    float pad = size * 0.15f;
    ImU32 colour = ImGui::GetColorU32(enabled ? kText : kDim);
    float left = std::floor(min.x + (width - size) * 0.5f);
    DrawIcon(draw, icon, ImVec2(left + pad, min.y + pad), size - pad * 2.0f, colour);
    draw->AddText(ImVec2(std::floor(min.x + (width - text.x) * 0.5f), min.y + size), colour, label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    ImGui::EndDisabled();
    ImGui::SameLine();
    return pressed;
}

void Gap() {
    float em = ImGui::GetFontSize();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float height = em * kIconEms + ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y), ImVec2(at.x, at.y + height), ImGui::GetColorU32(kLine));
    ImGui::Dummy(ImVec2(em * 0.3f, height));
    ImGui::SameLine();
}

// the toolbar's height
float DrawToolbar(float top, float width, const State& s) {
    float em = ImGui::GetFontSize();
    float height = em * kIconEms + ImGui::GetTextLineHeight() + em * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(0.0f, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.25f, em * 0.25f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.15f, 0.0f));
    if (ImGui::Begin("##toolbar", nullptr, kFixed)) {
        if (IconButton("##undo", Icon::Undo, "Undo", "Undo (Ctrl+Z)", false, s.undo_count > 0)) Undo(1);
        Gap();
        for (const ToolEntry& t : kToolEntries) {
            std::string tip = std::string(ToolName(t.tool)) + " (" + t.key + ")";
            ImGui::PushID(t.tool);
            if (IconButton("##tool", t.icon, ToolName(t.tool), tip.c_str(), s.tool == t.tool, true)) SelectTool(t.tool);
            ImGui::PopID();
        }
        Gap();
        for (int b = 0; b < kBrushCount; b++) {
            std::string tip = std::string("Terrain: ") + BrushName(b);
            ImGui::PushID(b);
            bool on = s.tool == kTerrain && s.brush == b;
            if (IconButton("##brush", kBrushIcons[b], BrushName(b), tip.c_str(), on, true)) SelectBrush(b);
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return height;
}

// Dock

void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// a slider with a number box beside it
bool SliderWithBox(const char* label, float* value, float min, float max, const char* format) {
    float em = ImGui::GetFontSize();
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(em * 7.0f);
    ImGui::SetNextItemWidth(-em * 5.0f);
    bool changed = ImGui::SliderFloat("##slider", value, min, max, format);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputFloat("##box", value, 0.0f, 0.0f, format);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    *value = std::clamp(*value, min, max);
    return changed;
}

void DrawTerrainParameters(const State& s) {
    float em = ImGui::GetFontSize();
    ImGui::Text("Brush: %s", BrushName(s.brush));
    // the editor's radius comes in steps
    int step = s.radius_step;
    char metres[32];
    std::snprintf(metres, sizeof(metres), "%.1f m", RadiusMetres(step));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Radius");
    ImGui::SameLine(em * 7.0f);
    ImGui::SetNextItemWidth(-em * 5.0f);
    if (ImGui::SliderInt("##radius", &step, 0, kRadiusSteps - 1, metres)) SetRadiusStep(step);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    int shown = step + 1;
    if (ImGui::InputInt("##radius_box", &shown, 0) && shown >= 1 && shown <= kRadiusSteps) SetRadiusStep(shown - 1);
    if (s.brush == kSetHeight || s.brush == kExtend) {
        float height = s.set_height;
        if (SliderWithBox("Height", &height, 0.0f, MaxSetHeight(), "%.0f")) SetSetHeight(height);
    }
    if (s.brush == kPaint) {
        std::vector<std::string> textures = PaintTextures();
        const char* shown = size_t(s.texture) < textures.size() ? textures[size_t(s.texture)].c_str() : "";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Texture");
        ImGui::SameLine(em * 7.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##texture", shown)) {
            for (int t = 0; t < int(textures.size()); t++) {
                if (textures[size_t(t)].empty()) continue;
                ImGui::PushID(t);
                if (ImGui::Selectable(textures[size_t(t)].c_str(), t == s.texture)) SetTexture(t);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    bool square = s.square_cursor;
    if (ImGui::Checkbox("Square cursor", &square)) SetSquareCursor(square);
}

// a stepped value shown as text, with its step number in a box
bool StepSlider(const char* label, int* step, int count, const char* text) {
    float em = ImGui::GetFontSize();
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(em * 7.0f);
    ImGui::SetNextItemWidth(-em * 5.0f);
    bool changed = ImGui::SliderInt("##slider", step, 0, count - 1, text);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    int shown = *step + 1;
    if (ImGui::InputInt("##box", &shown, 0) && shown >= 1 && shown <= count) {
        *step = shown - 1;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

void DrawEnvironmentParameters(const State& s) {
    if (!s.has_environment) return;
    int time = int(s.environment[kTimeOfDay]);
    if (StepSlider("Time of Day", &time, kTimesOfDay, TimeOfDayName(time))) SetEnvironment(kTimeOfDay, float(time));
    float thickness = s.environment[kFogThickness];
    if (SliderWithBox("Fog", &thickness, kMinFogThickness, kMaxFogThickness, "%.1f"))
        SetEnvironment(kFogThickness, thickness);
    int distance = int(s.environment[kFogDistance]);
    char metres[32];
    std::snprintf(metres, sizeof(metres), "%.0f m", FogDistanceMetres(distance));
    if (StepSlider("Fog Distance", &distance, kFogDistanceSteps, metres))
        SetEnvironment(kFogDistance, float(distance));
    for (int row = kSound1; row <= kSound3; row++) {
        int sound = int(s.environment[row]);
        char label[16];
        std::snprintf(label, sizeof(label), "Sound %d", row - kSound1 + 1);
        char text[16];
        std::snprintf(text, sizeof(text), "%d", sound);
        if (StepSlider(label, &sound, kSoundCount, text)) SetEnvironment(row, float(sound));
    }
}

void DrawObjectParameters(const State& s) {
    const char* what = s.tool == kAddObject ? "places the object picked" : "drags the object under it";
    ImGui::TextWrapped("A left click in the view %s.", what);
    if (s.tool == kEditObject) ImGui::TextWrapped("Del removes the selected object.");
}

void DrawToolParameters(const State& s) {
    Heading("TOOL PARAMETERS");
    ImGui::TextUnformatted(ToolName(s.tool));
    if (s.tool == kTerrain) DrawTerrainParameters(s);
    if (s.tool == kAddObject || s.tool == kEditObject) DrawObjectParameters(s);
    if (s.tool == kEnvironment) DrawEnvironmentParameters(s);
}

void DrawHistory(const State& s) {
    std::vector<std::string> history = History();
    if (history.empty()) ImGui::TextUnformatted("Nothing to undo.");
    // undoing an entry undoes every one after it too
    for (size_t i = 0; i < history.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::Selectable(history[i].c_str(), i + 1 == history.size())) Undo(int(history.size() - i));
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(s.undo_count == 0);
    if (ImGui::Button("Undo")) Undo(1);
    ImGui::EndDisabled();
}

bool Matches(const std::string& name, const std::string& search) {
    auto lower = [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    auto it = std::search(name.begin(), name.end(), search.begin(), search.end(),
                          [&](char a, char b) { return lower(a) == lower(b); });
    return it != name.end();
}

// a click picks the object for the Add Object tool, a click in the view then places it
void DrawObjectBrowser(const State& s, const std::vector<Category>& catalogue) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search", g_search, sizeof(g_search));
    std::string search = g_search;
    ImGui::BeginChild("##objects");
    for (size_t c = 0; c < catalogue.size(); c++) {
        const std::vector<std::string>& items = catalogue[c].items;
        if (items.empty()) continue;
        int category = int(c);
        if (!search.empty()) ImGui::SetNextItemOpen(true);
        if (!ImGui::CollapsingHeader(CategoryName(category))) continue;
        for (size_t i = 0; i < items.size(); i++) {
            if (!search.empty() && !Matches(items[i], search)) continue;
            bool picked = s.tool == kAddObject && s.category == category && s.item == int(i);
            ImGui::PushID(category * 4096 + int(i));
            if (ImGui::Selectable(items[i].c_str(), picked)) PickObject(category, int(i));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawEditorSettings(const State& s) {
    for (int i = 0; i < kOptionCount; i++) {
        bool on = s.options[i];
        if (ImGui::Checkbox(OptionName(i), &on)) SetOption(i, on);
    }
    if (editor::g_solo) ImGui::Checkbox("Markers", &g_show_markers);
}

void DrawBudget(const std::vector<Category>& catalogue) {
    float em = ImGui::GetFontSize();
    for (size_t c = 0; c < catalogue.size(); c++) {
        const Category& k = catalogue[c];
        if (k.limit <= 0) continue;
        char text[32];
        std::snprintf(text, sizeof(text), "%d / %d", k.used, k.limit);
        ImGui::TextUnformatted(CategoryName(int(c)));
        ImGui::SameLine(em * 10.0f);
        ImGui::ProgressBar(float(k.used) / float(k.limit), ImVec2(-1.0f, 0.0f), text);
    }
}

void DrawTabs(const State& s) {
    if (!ImGui::BeginTabBar("##dock_tabs")) return;
    std::vector<Category> catalogue = Catalogue();
    if (ImGui::BeginTabItem("Objects")) {
        DrawObjectBrowser(s, catalogue);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Settings")) {
        DrawEditorSettings(s);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Budget")) {
        DrawBudget(catalogue);
        ImGui::EndTabItem();
    }
    if (editor::g_solo && ImGui::BeginTabItem("Properties")) {
        editor::DrawObjectPanel();
        ImGui::EndTabItem();
    }
    if (editor::g_solo && ImGui::BeginTabItem("AI")) {
        editor::DrawAiPanel();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("History")) {
        DrawHistory(s);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// a bar on the dock's left edge drags its width
void DrawSplitter(float height, float max_width) {
    float em = ImGui::GetFontSize();
    ImGui::InvisibleButton("##splitter", ImVec2(em * kSplitterEms, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) g_dock_width -= ImGui::GetIO().MouseDelta.x;
    g_dock_width = std::clamp(g_dock_width, em * kMinDockEms, max_width);
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGui::IsItemActive() ? kHeading : kLine));
}

void DrawDock(float x, float top, float bottom, const State& s) {
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(g_dock_width, bottom - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool open = ImGui::Begin("##dock", nullptr, kFixed);
    ImGui::PopStyleVar();
    if (open) {
        float max_width = ImGui::GetIO().DisplaySize.x - ImGui::GetFontSize() * kMinViewEms;
        DrawSplitter(bottom - top, max_width);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginGroup();
        float parameters = (bottom - top) * kParametersShare;
        ImGui::BeginChild("##parameters", ImVec2(0.0f, parameters), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawToolParameters(s);
        ImGui::EndChild();
        ImGui::BeginChild("##tabs", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawTabs(s);
        ImGui::EndChild();
        ImGui::EndGroup();
    }
    ImGui::End();
}

void DrawControls() {
    if (!g_show_controls) return;
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 20.0f, 0.0f), ImGuiCond_Appearing);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Controls", &g_show_controls, kFlags)) {
        for (const auto& [what, keys] : kControls) {
            ImGui::TextUnformatted(what);
            ImGui::SameLine(em * 9.0f);
            ImGui::TextUnformatted(keys);
        }
    }
    ImGui::End();
}

// Title

std::mutex g_title_lock;
std::wstring g_title;

std::wstring Title() {
    if (!MapOpen()) return app::kGame.title;
    std::string name = MapName();
    if (name.empty()) name = CampaignLevel();
    std::wstring map(name.begin(), name.end());
    if (map.empty()) map = editor::g_solo ? L"Untitled Level" : L"Untitled Map";
    if (MapChanged()) map += L"*";
    const wchar_t* editor = editor::g_solo ? L"LEVEL EDITOR" : L"MAP EDITOR";
    return std::wstring(kGameName) + L" - " + editor + L" - " + map + L" - XEX Recomp Editor";
}

// SetWindowText from here would wait on the window thread, which can be waiting for this frame
void UpdateTitle() {
    std::wstring title = Title();
    {
        std::lock_guard lock(g_title_lock);
        if (title == g_title) return;
        g_title = title;
    }
    PostMessageW(static_cast<HWND>(app::Window()), kSetTitle, 0, 0);
}

// Status bar and view

void DrawStatus(float top, float width, double fps, const State& s) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top));
    ImGui::SetNextWindowSize(ImVec2(width, ImGui::GetIO().DisplaySize.y - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.5f, 0.0f));
    bool open = ImGui::Begin("##status", nullptr, kFixed);
    ImGui::PopStyleVar();
    if (open) {
        float em = ImGui::GetFontSize();
        ImGui::AlignTextToFramePadding();
        if (!s.applying) ImGui::TextUnformatted("Ready");
        else if (s.tool == kTerrain) ImGui::Text("Terrain: %s", BrushName(s.brush));
        else ImGui::Text("%s", ToolName(s.tool));
        ImGui::SameLine(width - em * 44);
        ImGui::Text("Cursor %.1f, %.1f, %.1f", s.cursor[0], s.cursor[1], s.cursor[2]);
        float camera[3];
        if (ViewPosition(camera)) {
            ImGui::SameLine(width - em * 24);
            ImGui::Text("Camera %.1f, %.1f, %.1f", camera[0], camera[1], camera[2]);
        }
        ImGui::SameLine(width - em * 6);
        ImGui::Text("%.0f FPS", fps);
    }
    ImGui::End();
}

// the mouse over the view: right drag looks, the wheel zooms, the left button uses the tool where it points
void UpdateViewInput(const Box& view) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    bool inside = m.x >= view.x && m.y >= view.y && m.x < view.x + view.w && m.y < view.y + view.h;
    bool over = inside && !io.WantCaptureMouse;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) g_looking = over;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_applying = over;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) g_looking = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_applying = false;
    if (g_looking) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    Box frame = FrameBox(io.DisplaySize.x, io.DisplaySize.y);
    ViewInput in;
    in.inside = over || g_looking || g_applying;
    in.u = frame.w > 0.0f ? (m.x - frame.x) / frame.w : 0.5f;
    in.v = frame.h > 0.0f ? (m.y - frame.y) / frame.h : 0.5f;
    if (g_looking) {
        in.look_x = io.MouseDelta.x;
        in.look_y = io.MouseDelta.y;
    }
    if (over) in.wheel = io.MouseWheel;
    in.apply = g_applying;
    SetViewInput(in);
}

// true while it takes the mouse: always but over the view while a console page has the editor's input
bool DrawShell(double fps) {
    UpdateTitle();
    g_editor_shell = ShellUp();  // no mouse look while editing, play tests keep it
    if (!ShellUp()) {
        SetViewBox({});
        SetViewInput({});
        g_editor_typing = false;
        return editor::g_solo && !MapOpen() && LevelListUp() && DrawLevelList();
    }
    State s = Read();
    float em = ImGui::GetFontSize();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (g_dock_width <= 0.0f) g_dock_width = em * kDockEms;
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    for (const auto& [col, color] : kShellColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    DrawMenuBar(s);
    float top = ImGui::GetFrameHeight();
    if (g_show_toolbar) top += DrawToolbar(top, size.x, s);
    float bottom = g_show_status ? size.y - ImGui::GetFrameHeight() : size.y;
    float dock = g_show_dock ? std::min(g_dock_width, size.x - em * kMinViewEms) : 0.0f;
    Box view{0.0f, top, size.x - dock, bottom - top};
    SetViewBox(view);
    bool console = ConsoleMenuUp();
    if (console) SetViewInput({});
    else UpdateViewInput(view);
    if (g_show_dock) DrawDock(size.x - dock, top, bottom, s);
    if (g_show_status) DrawStatus(bottom, size.x, fps, s);
    if (editor::g_solo && g_show_markers) editor::DrawMarkers();
    DrawControls();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(int(std::size(kPanelColors) + std::size(kShellColors)));
    g_editor_typing = ImGui::GetIO().WantTextInput;
    return !console || ImGui::GetIO().WantCaptureMouse;
}

// the level list takes the keys while up, Esc closes it; Ctrl+Z undoes, Ctrl+S saves a map, Del deletes, 1 to 4 pick
// the tools, F5 play tests
bool EditorKey(uintptr_t wp) {
    if (editor::g_solo && !MapOpen() && LevelListUp()) {
        if (wp == VK_ESCAPE) CloseLevelList();
        return true;
    }
    if (!MapOpen() || g_editor_typing) return false;
    if (wp == VK_F5) {
        PlayTest();
        return true;
    }
    if (!ShellUp() || ConsoleMenuUp()) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && wp == 'Z') {
        Undo(1);
        return true;
    }
    if (ctrl && wp == 'S') {
        Save();
        return true;
    }
    if (wp == VK_DELETE) {
        DeleteSelected();
        return true;
    }
    if (!ctrl && wp >= '1' && wp < '1' + kToolCount) {
        SelectTool(int(wp - '1'));
        return true;
    }
    return false;
}

bool EditorMessage(void* hwnd, uint32_t msg, uintptr_t wp) {
    static bool maximised = false;  // the first show only, a window the user sizes stays so
    if (msg == WM_SHOWWINDOW && wp && !maximised && app::Get().display_mode == app::DisplayMode::Windowed) {
        maximised = true;
        PostMessageW(static_cast<HWND>(hwnd), WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        return false;
    }
    if (msg == kSetTitle) {
        std::lock_guard lock(g_title_lock);
        SetWindowTextW(static_cast<HWND>(hwnd), g_title.c_str());
        return true;
    }
    return msg == WM_KEYDOWN && EditorKey(wp);
}

const bool g_registered = (g_editor_ui = DrawShell, g_editor_message = EditorMessage, true);

}  // namespace
