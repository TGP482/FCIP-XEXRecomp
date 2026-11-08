// the options panel on F10
#include "common/menu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>

#include "app/app.h"
#include "common/panel_colors.h"
#include "core/input.h"

namespace fcip {

namespace {

using rows::Kind;
using rows::Row;

std::atomic<bool> g_open{false};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, app::kFpsCapRefresh, 0};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = StepIndex(on, step, 2) != 0;
}

enum RowId { kDisplayMode, kResolution, kRenderResolution, kVsync, kMaxFps, kAniso };

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY", nullptr},
    {Kind::Value, kDisplayMode, "DISPLAY MODE", "Switch between fullscreen, borderless and windowed"},
    {Kind::Value, kResolution, "RESOLUTION", "Every mode your monitor reports, defaulting to your desktop resolution"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION",
     "Renders the game at 20% to 200% of the output resolution, independent of the window size"},
    {Kind::Value, kVsync, "VERTICAL SYNC", "Reduces screen tearing"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE",
     "Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap"},
    {Kind::Heading, -1, "GRAPHICS", nullptr},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING", "Forces the selected anisotropic filtering level on all textures"},
    {Kind::Defaults, -1, "RESTORE DEFAULTS", "Puts every setting on this page back to its default"},
};

bool DisplayRow(int id) { return id == kDisplayMode || id == kResolution; }

// the window's monitor's modes smallest first, so Left steps down
const std::vector<app::Mode>& Modes() {
    static const std::vector<app::Mode> modes = [] {
        std::vector<app::Mode> out;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
            if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
            app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
            auto same = [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; };
            if (std::none_of(out.begin(), out.end(), same)) out.push_back(m);
        }
        std::sort(out.begin(), out.end(), [](const app::Mode& a, const app::Mode& b) {
            return a.width != b.width ? a.width < b.width : a.height < b.height;
        });
        return out;
    }();
    return modes;
}

// the mode shown, stepped first; the desktop's own mode is kept as 0, which follows the desktop
app::Mode StepResolution(app::Settings& s, int step) {
    const std::vector<app::Mode>& modes = Modes();
    app::Mode desktop = app::Desktop();
    app::Mode m = s.width && s.height ? app::Mode{s.width, s.height} : desktop;
    int i = 0;
    for (size_t k = 0; k < modes.size(); k++) {
        if (modes[k].width == m.width && modes[k].height == m.height) i = int(k);
    }
    if (step && !modes.empty()) {
        m = modes[size_t(StepIndex(i, step, int(modes.size())))];
        bool native = m.width == desktop.width && m.height == desktop.height;
        s.width = native ? 0 : m.width;
        s.height = native ? 0 : m.height;
    }
    return m;
}

void StepRow(int id, int step, app::Settings& s, char (&text)[64]) {
    switch (id) {
        case kDisplayMode: {
            static const char* const kModes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
            int count = int(std::size(kModes));
            if (step) s.display_mode = app::DisplayMode(StepIndex(int(s.display_mode), step, count));
            std::snprintf(text, sizeof(text), "%s", kModes[std::min(int(s.display_mode), count - 1)]);
            break;
        }
        case kResolution: {
            app::Mode m = StepResolution(s, step);
            std::snprintf(text, sizeof(text), "%ux%u", m.width, m.height);
            break;
        }
        case kRenderResolution: {
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            app::Mode out = StepResolution(s, 0);
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, out.width * s.render_percent / 100,
                          out.height * s.render_percent / 100);
            break;
        }
        case kVsync:
            Flip(s.vsync, step);
            std::snprintf(text, sizeof(text), "%s", OnOff(s.vsync));
            break;
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == app::kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            break;
        }
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            break;
    }
}

void Changed(bool display) {
    if (display) app::ApplyDisplay();
    app::Save();
}

// on a copy of the settings a step is only shown, not saved
std::string RowText(int id, int step, app::Settings& s = app::Get()) {
    char text[64] = "";
    StepRow(id, step, s, text);
    if (step && &s == &app::Get()) Changed(DisplayRow(id));
    return text;
}

void RestoreDefaults(int page) {
    if (page != kSettingsPage) return;
    const app::Settings def;
    app::Settings& s = app::Get();
    s.display_mode = def.display_mode;
    s.width = def.width;
    s.height = def.height;
    s.render_percent = def.render_percent;
    s.vsync = def.vsync;
    s.fps_cap = def.fps_cap;
    s.aniso = def.aniso;
    Changed(true);
}

// Panel

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// label left, arrows and value right, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize();
    float row_w = ImGui::GetContentRegionAvail().x;
    float row_h = ImGui::GetFrameHeight();
    float value_w = em * kValueEms;
    float block_w = row_h * 2 + value_w + style.ItemSpacing.x * 2;
    float row_x = ImGui::GetCursorPosX();
    float row_y = ImGui::GetCursorPosY();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row_w, row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    ImGui::SetCursorPos(ImVec2(row_x + style.ItemInnerSpacing.x, row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = row_x + row_w - block_w;
    ImGui::SetCursorPos(ImVec2(block_x, row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + row_h + style.ItemSpacing.x;
    float pad = (value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + value_w + style.ItemSpacing.x, row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    // an item, not a cursor move, so the child window grows to the row
    ImGui::SetCursorPos(ImVec2(row_x, row_y));
    ImGui::Dummy(ImVec2(row_w, row_h));
    return step;
}

const char* const kPages[] = {"RECOMP SETTINGS"};
std::atomic<int> g_page{kSettingsPage};
std::atomic<bool> g_page_set{true};  // the tab bar picks g_page on its next frame

// on F10 the display mode and resolution wait for Apply, as Windows' display settings do; closing drops them
bool g_display_held = false;
app::Settings g_display;

void HoldDisplay(int id, int step) {
    const app::Settings& s = app::Get();
    if (!g_display_held) {
        g_display.display_mode = s.display_mode;
        g_display.width = s.width;
        g_display.height = s.height;
    }
    RowText(id, step, g_display);
    g_display_held = g_display.display_mode != s.display_mode || g_display.width != s.width ||
                     g_display.height != s.height;
}

void Apply() {
    app::Settings& s = app::Get();
    s.display_mode = g_display.display_mode;
    s.width = g_display.width;
    s.height = g_display.height;
    g_display_held = false;
    Changed(true);
}

void DrawRows(int page) {
    for (const Row& r : rows::Page(page)) {
        if (r.kind == Kind::Heading) {
            Section(r.label);
            continue;
        }
        if (r.kind != Kind::Value) continue;
        bool held = g_display_held && DisplayRow(r.id);
        int step = OptionRow(r.label, RowText(r.id, 0, held ? g_display : app::Get()).c_str());
        if (step && DisplayRow(r.id)) HoldDisplay(r.id, step);
        else if (step) RowText(r.id, step);
    }
}

// one tab a page, as wide as they share the line; LB / RB step them as the game's menus do
void DrawTabs() {
    constexpr int kCount = int(std::size(kPages));
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    if (step) {
        g_page = StepIndex(g_page, step, kCount);
        g_page_set = true;
    }
    // a page set here or from the game's menus shows from the next frame, the tab drawn this one still reads as open
    bool set = g_page_set.exchange(false);
    ImGui::PushStyleColor(ImGuiCol_Tab, kTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kTabHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##pages")) {
        float gaps = ImGui::GetStyle().ItemInnerSpacing.x * float(kCount - 1);
        float w = (ImGui::GetContentRegionAvail().x - gaps) / float(kCount);
        for (int i = 0; i < kCount; i++) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginTabItem(kPages[i], nullptr, set && i == g_page ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!set) g_page = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 50, em * 32), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        const char* title = "SETTINGS";
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::TextUnformatted(title);
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        DrawRows(g_page);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Up and Down select, Left and Right change, Escape closes");
        ImGui::PopStyleColor();
        float bw = em * 8;
        float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
        if (ImGui::Button("Back", ImVec2(bw, 0))) ToggleMenu();
        ImGui::SameLine();
        if (ImGui::Button("Defaults", ImVec2(bw, 0))) {
            g_display_held = false;
            RestoreDefaults(g_page);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_display_held);
        if (ImGui::Button("Apply", ImVec2(bw, 0))) Apply();
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleMenu() {
    std::lock_guard lock(app::UiLock());
    g_open = !g_open;
    g_display_held = false;
}

void OpenMenuPage(int page) {
    std::lock_guard lock(app::UiLock());
    g_page = std::clamp(page, 0, int(std::size(kPages)) - 1);
    g_page_set = true;
    g_display_held = false;
    g_open = true;
}

void DrawUi(double) {
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (MenuOpen()) DrawPanel();
}

// F10 opens and closes the panel, Escape closes it; the game sees neither while it is open
bool MenuMessage(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    bool key = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (!key || (wp != VK_F10 && !(wp == VK_ESCAPE && MenuOpen()))) return false;
    ToggleMenu();
    *result = 0;
    return true;
}

void MenuPad(int, input::Pad& pad) {
    if (MenuOpen()) pad = {.connected = pad.connected};
}

namespace rows {

std::span<const Row> Page(int page) {
    if (page == kSettingsPage) return kSettingsRows;
    return {};
}

std::string Text(const Row& row, int step) {
    std::lock_guard lock(app::UiLock());
    if (row.kind != Kind::Value) return {};
    return RowText(row.id, step);
}

void Defaults(int page) {
    std::lock_guard lock(app::UiLock());
    RestoreDefaults(page);
}

}  // namespace rows

}  // namespace fcip
