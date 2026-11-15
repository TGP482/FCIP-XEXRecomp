// ige.xex, the solo level editor: the logic objects (triggers, checkpoints, the level end, waypoints) have no model, so
// while the Add or Edit Object tool runs each gets a square and its name over the frame. They are gathered from the
// entity system on the editor's thread and projected with the camera the last frame drew. A campaign level's AI objects
// get a circle, shapes and paths their lines, named when near or picked in the AI tab.
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
#include "leveleditor/ai_panel.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// the catalogue's logic classes
constexpr const char* kLogicClasses[] = {"Checkpoint",      "EndMapTrigger",  "ProximityTrigger", "AreaTrigger",
                                         "DelayTrigger",    "MultipleTrigger", "UtilityTrigger",   "Waypoint"};

constexpr float kMarkerHalf = 6.0f;  // pixels
constexpr float kLineWidth = 2.0f;
constexpr float kMinClipW = 0.01f;        // behind the camera below this
constexpr float kAiNamedWithin = 120.0f;  // metres from the camera
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

struct View {
    const float* clip;
    fcip::Box frame;
    ImDrawList* draw;
};

// false behind the camera
bool Project(const View& v, const float p[3], ImVec2& out) {
    const float* c = v.clip;
    float x = p[0] * c[0] + p[1] * c[4] + p[2] * c[8] + c[12];
    float y = p[0] * c[1] + p[1] * c[5] + p[2] * c[9] + c[13];
    float w = p[0] * c[3] + p[1] * c[7] + p[2] * c[11] + c[15];
    if (w < kMinClipW) return false;
    out = ImVec2(v.frame.x + (x / w * 0.5f + 0.5f) * v.frame.w, v.frame.y + (0.5f - y / w * 0.5f) * v.frame.h);
    return true;
}

void DrawLogicMarkers(const View& v) {
    std::vector<Marker> markers;
    {
        std::lock_guard lock(g_lock);
        if (std::chrono::steady_clock::now() - g_updated > kShownFor) return;
        markers = g_markers;
    }
    ImU32 colour = ImGui::GetColorU32(fcip::kHeading);
    for (const Marker& m : markers) {
        ImVec2 at;
        if (!Project(v, m.pos, at)) continue;
        v.draw->AddRect(ImVec2(at.x - kMarkerHalf, at.y - kMarkerHalf), ImVec2(at.x + kMarkerHalf, at.y + kMarkerHalf),
                        colour, 0.0f, 0, kLineWidth);
        v.draw->AddText(ImVec2(at.x + kMarkerHalf * 1.5f, at.y - kMarkerHalf), colour, m.label.c_str());
    }
}

void DrawPoints(const View& v, const AiObject& o, ImU32 colour) {
    for (size_t i = 1; i < o.points.size(); i++) {
        ImVec2 a;
        ImVec2 b;
        if (Project(v, o.points[i - 1].data(), a) && Project(v, o.points[i].data(), b)) {
            v.draw->AddLine(a, b, colour, kLineWidth);
        }
    }
}

bool Near(const float eye[3], const float p[3]) {
    float dx = p[0] - eye[0];
    float dy = p[1] - eye[1];
    float dz = p[2] - eye[2];
    return dx * dx + dy * dy + dz * dz < kAiNamedWithin * kAiNamedWithin;
}

void DrawAiMarkers(const View& v) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    float eye[3] = {};
    bool have_eye = fcip::ViewPosition(eye);
    uint32_t picked = SelectedAiObject();
    for (const AiObject& o : ai->objects) {
        ImU32 colour = ImGui::GetColorU32(o.node == picked ? fcip::kText : fcip::kDim);
        DrawPoints(v, o, colour);
        ImVec2 at;
        if (!o.placed || !Project(v, o.pos, at)) continue;
        v.draw->AddCircle(at, kMarkerHalf, colour, 0, kLineWidth);
        if (o.node != picked && !(have_eye && Near(eye, o.pos))) continue;
        v.draw->AddText(ImVec2(at.x + kMarkerHalf * 1.5f, at.y - kMarkerHalf), colour, o.name.c_str());
    }
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
    if (!ViewClip(clip)) return;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    View view{clip, FrameBox(size.x, size.y), ImGui::GetBackgroundDrawList()};
    view.draw->PushClipRect(ImVec2(view.frame.x, view.frame.y),
                            ImVec2(view.frame.x + view.frame.w, view.frame.y + view.frame.h));
    DrawAiMarkers(view);
    DrawLogicMarkers(view);
    view.draw->PopClipRect();
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_820D6C38);

// the Add Object tool's update, each frame while it runs
PPC_FUNC(sub_820D6C38) {
    if (!fcip::editor::g_solo) return __imp__sub_820D6C38(ctx, base);
    __imp__sub_820D6C38(ctx, base);
    fcip::editor::UpdateMarkers(ctx);
}
