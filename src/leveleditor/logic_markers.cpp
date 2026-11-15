// ige.xex, the solo level editor: the logic objects (triggers, checkpoints, the level end, waypoints) have no model, so
// while the Add or Edit Object tool runs each gets a square and its name over the frame. They are gathered from the
// entity system on the editor's thread and projected with the camera the last frame drew.
#include "leveleditor/markers.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/aspect.h"
#include "common/panel_colors.h"
#include "common/view_clip.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// the catalogue's logic classes
constexpr const char* kLogicClasses[] = {"Checkpoint",      "EndMapTrigger",  "ProximityTrigger", "AreaTrigger",
                                         "DelayTrigger",    "MultipleTrigger", "UtilityTrigger",   "Waypoint"};

constexpr float kMarkerHalf = 6.0f;  // pixels
constexpr float kMinClipW = 0.01f;   // behind the camera below this
constexpr auto kShownFor = std::chrono::milliseconds(250);  // after the tools stop updating

struct Marker {
    float pos[3];
    std::string label;
};

std::mutex g_lock;
std::vector<Marker> g_markers;
std::chrono::steady_clock::time_point g_updated;

bool IsLogic(const std::string& cls) {
    return std::find(std::begin(kLogicClasses), std::end(kLogicClasses), cls) != std::end(kLogicClasses);
}

void AddIfLogic(PPCContext& ctx, uint32_t entity, std::vector<Marker>& out) {
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    if (!cls || !IsLogic(ReadCString(cls))) return;
    uint32_t pos = uint32_t(CallGuest(ctx, Method(entity, kEntityPosition), {entity, 1}));
    if (!pos) return;
    out.push_back({{GF(pos), GF(pos + 4), GF(pos + 8)}, GuestString(entity + kEntityName)});
}

}  // namespace

namespace fcip::editor {

void UpdateMarkers(PPCContext& ctx) {
    std::vector<Marker> found;
    ForEachEntity([&](uint32_t entity) { AddIfLogic(ctx, entity, found); });
    std::lock_guard lock(g_lock);
    g_markers.swap(found);
    g_updated = std::chrono::steady_clock::now();
}

void DrawMarkers() {
    float clip[16];
    std::vector<Marker> markers;
    {
        std::lock_guard lock(g_lock);
        if (std::chrono::steady_clock::now() - g_updated > kShownFor) return;
        markers = g_markers;
    }
    if (markers.empty() || !ViewClip(clip)) return;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    Box frame = FrameBox(size.x, size.y);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(ImVec2(frame.x, frame.y), ImVec2(frame.x + frame.w, frame.y + frame.h));
    ImU32 colour = ImGui::GetColorU32(kHeading);
    for (const Marker& m : markers) {
        const float* p = m.pos;
        float x = p[0] * clip[0] + p[1] * clip[4] + p[2] * clip[8] + clip[12];
        float y = p[0] * clip[1] + p[1] * clip[5] + p[2] * clip[9] + clip[13];
        float w = p[0] * clip[3] + p[1] * clip[7] + p[2] * clip[11] + clip[15];
        if (w < kMinClipW) continue;
        ImVec2 at(frame.x + (x / w * 0.5f + 0.5f) * frame.w, frame.y + (0.5f - y / w * 0.5f) * frame.h);
        draw->AddRect(ImVec2(at.x - kMarkerHalf, at.y - kMarkerHalf), ImVec2(at.x + kMarkerHalf, at.y + kMarkerHalf),
                      colour, 0.0f, 0, 2.0f);
        draw->AddText(ImVec2(at.x + kMarkerHalf * 1.5f, at.y - kMarkerHalf), colour, m.label.c_str());
    }
    draw->PopClipRect();
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_820D6C38);

// the Add Object tool's update, each frame while it runs
PPC_FUNC(sub_820D6C38) {
    if (!fcip::editor::g_solo) return __imp__sub_820D6C38(ctx, base);
    __imp__sub_820D6C38(ctx, base);
    fcip::editor::UpdateMarkers(ctx);
}
