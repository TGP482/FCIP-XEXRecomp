// ige.xex, the solo level editor: the editor view shows what the game uses to run a level, which has no model of its
// own. Each editing frame the entities are read on the editor's thread (class, name, place, the box the engine tests
// them with, the Properties of the classes a category needs); the window thread draws each shown category over the
// frame. Waypoints get a square and their name. A campaign level's AI objects get a circle, shapes and paths their
// lines, named when near or picked in the AI tab.
#include "leveleditor/markers.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "kernel/memory.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/guest.h"
#include "leveleditor/object_panel.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kCategoryNames[] = {"Player Start", "Triggers", "Sequences", "Logic Objects", "AI Objects"};
static_assert(std::size(kCategoryNames) == kViewCategoryCount);

// the catalogue's logic classes the later categories do not draw yet
constexpr const char* kLogicClasses[] = {"Waypoint"};
constexpr int kPropertyReadsPerFrame = 32;  // a level's first frames read them in turns

constexpr float kCircleRadius = 6.0f;  // pixels
constexpr float kLineWidth = 2.0f;
constexpr float kMaxBoxSide = 10000.0f;  // the engine drops an area this big (1e8 squared)

// guest scratch: GetBBox's two Vec3
constexpr uint32_t kScratchMin = 0x00;
constexpr uint32_t kScratchMax = 0x0C;
constexpr uint32_t kScratchBytes = 0x18;

std::mutex g_lock;
std::shared_ptr<const std::vector<ViewEntity>> g_entities = std::make_shared<std::vector<ViewEntity>>();
// editor thread
uint32_t g_scratch = 0;
std::unordered_map<uint32_t, std::vector<XmlAttribute>> g_properties;  // by entity id

void ReadVec(uint32_t at, float out[3]) {
    for (uint32_t i = 0; i < 3; i++) out[i] = GF(at + i * 4);
}

// a box with no volume, or one the engine would not use, is none
bool UsableBox(const float min[3], const float max[3]) {
    bool volume = false;
    for (int i = 0; i < 3; i++) {
        if (max[i] < min[i] || max[i] - min[i] > kMaxBoxSide) return false;
        volume |= max[i] > min[i];
    }
    return volume;
}

ViewEntity ReadEntity(PPCContext& ctx, uint32_t entity) {
    ViewEntity e;
    e.id = R32(entity + kEntityId);
    e.name = GuestString(entity + kEntityName);
    if (uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}))) e.cls = ReadCString(cls);
    if (uint32_t pos = uint32_t(CallGuest(ctx, Method(entity, kEntityPosition), {entity, 1}))) ReadVec(pos, e.pos);
    ReadVec(entity + kEntityAngles, e.angles);
    CallGuest(ctx, Method(entity, kEntityBBox), {entity, g_scratch + kScratchMin, g_scratch + kScratchMax});
    ReadVec(g_scratch + kScratchMin, e.box_min);
    ReadVec(g_scratch + kScratchMax, e.box_max);
    e.has_box = UsableBox(e.box_min, e.box_max);
    return e;
}

template <size_t N>
bool IsOneOf(const std::string& cls, const char* const (&classes)[N]) {
    return std::find(std::begin(classes), std::end(classes), cls) != std::end(classes);
}

// the classes whose Properties a category shows
bool WantsProperties(const std::string& cls) { return IsTrigger(cls); }

// read once per entity, a few each frame
void LatchProperties(PPCContext& ctx, uint32_t entity, ViewEntity& e, int& reads) {
    auto found = g_properties.find(e.id);
    if (found == g_properties.end()) {
        if (reads == 0) return;
        reads--;
        found = g_properties.emplace(e.id, ReadProperties(ctx, entity)).first;
    }
    e.properties = found->second;
}

void DrawLogicMarkers(const View& v) {
    ImU32 colour = ImGui::GetColorU32(fcip::kHeading);
    for (const ViewEntity& e : *ViewEntities()) {
        if (!IsOneOf(e.cls, kLogicClasses)) continue;
        Square(v, e.pos, colour);
        Label(v, e.pos, e.name.c_str(), colour);
    }
}

void DrawPoints(const View& v, const AiObject& o, ImU32 colour) {
    for (size_t i = 1; i < o.points.size(); i++) Line(v, o.points[i - 1].data(), o.points[i].data(), colour);
}

void DrawAiMarkers(const View& v) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    uint32_t picked = SelectedAiObject();
    for (const AiObject& o : ai->objects) {
        // the player start's, and the trigger areas'
        if (o.type == "Respawn" || !o.entities.empty()) continue;
        ImU32 colour = ImGui::GetColorU32(o.node == picked ? fcip::kText : fcip::kDim);
        DrawPoints(v, o, colour);
        ImVec2 at;
        if (!o.placed || !Project(v, o.pos, at)) continue;
        v.draw->AddCircle(at, kCircleRadius, colour, 0, kLineWidth);
        if (o.node != picked && !NearCamera(v, o.pos)) continue;
        Label(v, o.pos, o.name.c_str(), colour);
    }
}

}  // namespace

namespace fcip::editor {

const char* ViewCategoryName(int category) { return kCategoryNames[category]; }

std::shared_ptr<const std::vector<ViewEntity>> ViewEntities() {
    std::lock_guard lock(g_lock);
    return g_entities;
}

std::string PropertyOf(const ViewEntity& e, const char* key) {
    for (const XmlAttribute& a : e.properties) {
        if (a.key == key) return a.value;
    }
    return {};
}

void UpdateMarkers(PPCContext& ctx) {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    if (!g_scratch) return;
    auto found = std::make_shared<std::vector<ViewEntity>>();
    int reads = kPropertyReadsPerFrame;
    ForEachEntity([&](uint32_t entity) {
        ViewEntity e = ReadEntity(ctx, entity);
        if (WantsProperties(e.cls)) LatchProperties(ctx, entity, e, reads);
        found->push_back(std::move(e));
    });
    std::lock_guard lock(g_lock);
    g_entities = std::move(found);
}

void ForgetProperties(uint32_t entity_id) { g_properties.erase(entity_id); }

void ForgetMarkers() {
    g_properties.clear();
    std::lock_guard lock(g_lock);
    g_entities = std::make_shared<std::vector<ViewEntity>>();
}

void DrawMarkers() {
    View view;
    if (!BeginView(view)) return;
    if (g_view_shown[kViewAi]) DrawAiMarkers(view);
    if (g_view_shown[kViewLogic]) DrawLogicMarkers(view);
    if (g_view_shown[kViewTriggers]) DrawTriggers(view);
    if (g_view_shown[kViewSequences]) DrawSequences(view);
    if (g_view_shown[kViewPlayerStart]) DrawPlayerStart(view);
    EndView(view);
}

}  // namespace fcip::editor
