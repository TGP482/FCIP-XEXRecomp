// ige.xex: once a map is open FCIPEditor.exe is a desktop editor, the 3D view between a menu bar, a dock on the right
// and a status bar. The frame is drawn into the view alone (SetViewBox), at the view's shape.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>

#include <imgui.h>

#include "common/aspect.h"
#include "common/menu.h"
#include "common/panel_colors.h"
#include "common/view_clip.h"
#include "kernel/kernel.h"
#include "leveleditor/markers.h"
#include "leveleditor/mode.h"
#include "leveleditor/object_panel.h"

using namespace fcip;

namespace {

// EditorInterfaceHandler, a static object: +0x104 is 1 while editing, 2 in first person
constexpr uint32_t kEditorHandler = 0x8296F758;
constexpr uint32_t kHandlerMode = 0x104;
constexpr uint32_t kModeEditing = 1;

constexpr float kDockEms = 30.0f;  // the dock's first width
constexpr float kMinDockEms = 16.0f;
constexpr float kMinViewEms = 20.0f;
constexpr float kSplitterEms = 0.4f;

constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNav;

constexpr std::pair<ImGuiCol, ImVec4> kShellColors[] = {
    {ImGuiCol_MenuBarBg, kTab}, {ImGuiCol_PopupBg, kPanelBg}, {ImGuiCol_CheckMark, kText},
};

std::atomic<bool> g_map_open{false};  // from the main editor showing until the map manager does

// window thread
float g_dock_width = 0.0f;  // pixels, 0 until the first frame
bool g_show_dock = true;
bool g_show_status = true;

bool ShellUp() { return g_map_open && kernel::R32(kEditorHandler + kHandlerMode) == kModeEditing; }

float DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return 0.0f;
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Dock", nullptr, &g_show_dock);
        ImGui::MenuItem("Status Bar", nullptr, &g_show_status);
        ImGui::EndMenu();
    }
    float height = ImGui::GetWindowSize().y;
    ImGui::EndMainMenuBar();
    return height;
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

void DrawDock(float x, float top, float bottom) {
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(g_dock_width, bottom - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool open = ImGui::Begin("##dock", nullptr, kFixed);
    ImGui::PopStyleVar();
    if (open) {
        float max_width = ImGui::GetIO().DisplaySize.x - ImGui::GetFontSize() * kMinViewEms;
        DrawSplitter(bottom - top, max_width);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##dock_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (editor::g_solo) editor::DrawObjectPanel();
        ImGui::EndChild();
    }
    ImGui::End();
}

void DrawStatus(float top, float width, double fps) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top));
    ImGui::SetNextWindowSize(ImVec2(width, ImGui::GetIO().DisplaySize.y - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 0.5f, 0.0f));
    bool open = ImGui::Begin("##status", nullptr, kFixed);
    ImGui::PopStyleVar();
    if (open) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ready");
        float em = ImGui::GetFontSize();
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

// true while the pointer is on the panels
bool DrawShell(double fps) {
    if (!ShellUp()) {
        SetViewBox({});
        g_editor_typing = false;
        return false;
    }
    float em = ImGui::GetFontSize();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (g_dock_width <= 0.0f) g_dock_width = em * kDockEms;
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    for (const auto& [col, color] : kShellColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    float top = DrawMenuBar();
    float bottom = g_show_status ? size.y - ImGui::GetFrameHeight() : size.y;
    float dock = g_show_dock ? std::min(g_dock_width, size.x - em * kMinViewEms) : 0.0f;
    SetViewBox({0.0f, top, size.x - dock, bottom - top});
    if (g_show_dock) DrawDock(size.x - dock, top, bottom);
    if (g_show_status) DrawStatus(bottom, size.x, fps);
    if (editor::g_solo) editor::DrawMarkers();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(int(std::size(kPanelColors) + std::size(kShellColors)));
    g_editor_typing = ImGui::GetIO().WantTextInput;
    return ImGui::GetIO().WantCaptureMouse;
}

const bool g_registered = (g_editor_ui = DrawShell, true);

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_820C0340);
PPC_EXTERN_FUNC(__imp__sub_820CABB8);

// an editor interface handler (r3) becomes active: the main editor's once a map is open
PPC_FUNC(sub_820C0340) {
    if (uint32_t(ctx.r[3]) == kEditorHandler) g_map_open = true;
    __imp__sub_820C0340(ctx, base);
}

// the map manager becomes active: no map is open
PPC_FUNC(sub_820CABB8) {
    g_map_open = false;
    __imp__sub_820CABB8(ctx, base);
}
