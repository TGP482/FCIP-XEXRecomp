// ige.xex, the solo level editor: a left click with the Edit Object tool picks the nearest marker the view shows
// within a few pixels. An entity is selected in the tool as the editor selects what its camera points at (820DD780),
// so moving, rotating, deleting and the Properties tab work on it; a mission object is picked in the AI tab. A click
// that hits no marker goes to the tool as before.
#include "leveleditor/picking.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

#include <imgui.h>

#include "kernel/memory.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/guest.h"
#include "leveleditor/level_objects.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr float kPickPixels = 12.0f;
constexpr ImU32 kPickedColour = IM_COL32(255, 255, 255, 255);

// Edit Object tool: +0x108 a move or turn under way, +0x114 the selection vector (first at +0x118)
constexpr uint32_t kToolDragging = 0x108;
constexpr uint32_t kToolSelection = 0x114;
// editor objects by entity (std::map at 0x828C62F0, its end node at 0x828C6300): node key +0xC, object +0x10
constexpr uint32_t kObjectsByEntity = 0x828C62F0;
constexpr uint32_t kObjectsByEntityEnd = 0x828C6300;
constexpr uint32_t kMapNodeObject = 0x10;
constexpr uint32_t kObjectPicked = 0xC0;  // editor object method
// guest functions
constexpr uint32_t kFindObject = 0x8210CAA8;      // (out node, map, key*)
constexpr uint32_t kClearSelection = 0x820DD830;  // (tool)
constexpr uint32_t kPushBack = 0x820CE908;        // (vector, value*)
constexpr uint32_t kSelectionChanged = 0x820DDB68;  // (tool)
// guest scratch: the found node, the key, the pushed object
constexpr uint32_t kScratchNode = 0x0;
constexpr uint32_t kScratchKey = 0x4;
constexpr uint32_t kScratchBytes = 0x8;

std::atomic<uint32_t> g_pick_entity{0};  // entity id the editor thread selects, 0 for none
DockTab g_tab = kNoTab;
uint32_t g_scratch = 0;

// the category whose markers show the class, -1 for none
int CategoryOf(const std::string& cls) {
    for (const LevelClass& c : kLevelClasses) {
        if (cls == c.cls && c.category != kPickupsCategory) return c.category;
    }
    if (cls == "Checkpoint") return kViewPlayerStart;
    if (IsTrigger(cls)) return kViewTriggers;
    if (IsAiEntity(cls)) return kViewAi;
    if (IsSoundOrLight(cls)) return cls == "DynamicLight" ? kViewLights : kViewSounds;
    if (IsGameplay(cls)) return kViewGameplay;
    return -1;
}

int CategoryOf(const AiObject& o) {
    for (const LevelObject& k : kLevelObjects) {
        if (o.type == k.type) return k.category;
    }
    return kViewAi;
}

struct Hit {
    float distance = kPickPixels;
    uint32_t entity = 0;
    uint32_t object = 0;
};

// the candidate when it lands nearer the mouse than the best so far
void Consider(const View& v, ImVec2 mouse, const float p[3], Hit candidate, Hit& best) {
    ImVec2 at;
    if (!Project(v, p, at)) return;
    candidate.distance = std::hypot(at.x - mouse.x, at.y - mouse.y);
    if (candidate.distance < best.distance) best = candidate;
}

Hit Nearest(const View& v, ImVec2 mouse) {
    Hit best;
    for (const ViewEntity& e : *ViewEntities()) {
        int category = CategoryOf(e.cls);
        if (category >= 0 && g_view_shown[category]) Consider(v, mouse, e.pos, {0.0f, e.id, 0}, best);
    }
    for (const AiObject& o : ReadAiObjects()->objects) {
        if (!g_view_shown[CategoryOf(o)]) continue;
        if (o.placed) Consider(v, mouse, o.pos, {0.0f, 0, o.node}, best);
        for (const auto& p : o.points) Consider(v, mouse, p.data(), {0.0f, 0, o.node}, best);
    }
    return best;
}

uint32_t EntityById(uint32_t id) {
    uint32_t found = 0;
    ForEachEntity([&](uint32_t entity) {
        if (R32(entity + kEntityId) == id) found = entity;
    });
    return found;
}

uint32_t EditorObjectOf(PPCContext& ctx, uint32_t entity) {
    W32(g_scratch + kScratchKey, entity);
    CallGuest(ctx, kFindObject, {g_scratch + kScratchNode, kObjectsByEntity, g_scratch + kScratchKey});
    uint32_t node = R32(g_scratch + kScratchNode);
    return node && node != R32(kObjectsByEntityEnd) ? R32(node + kMapNodeObject) : 0;
}

}  // namespace

namespace fcip::editor {

bool PickMarker(float x, float y) {
    View v;
    Hit hit;
    if (ReadView(v)) hit = Nearest(v, ImVec2(x, y));
    SelectAiObject(hit.object);
    if (hit.object) g_tab = kAiTab;
    if (hit.entity) g_tab = kPropertiesTab;
    if (hit.entity) g_pick_entity = hit.entity;
    return hit.entity || hit.object;
}

DockTab TakeDockTab() {
    DockTab tab = g_tab;
    g_tab = kNoTab;
    return tab;
}

void DrawPicked(const View& v) {
    uint32_t picked = SelectedAiObject();
    for (const AiObject& o : ReadAiObjects()->objects) {
        if (o.node != picked) continue;
        if (o.placed) Square(v, o.pos, kPickedColour);
        if (!o.points.empty()) Prism(v, o.points, 0.0f, true, kPickedColour);
    }
}

void ApplyPick(PPCContext& ctx, uint32_t tool) {
    uint32_t id = g_pick_entity.exchange(0);
    if (!id || R32(tool + kToolDragging)) return;
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    uint32_t entity = g_scratch ? EntityById(id) : 0;
    uint32_t object = entity ? EditorObjectOf(ctx, entity) : 0;
    if (!object) return;
    CallGuest(ctx, kClearSelection, {tool});
    CallGuest(ctx, Method(object, kObjectPicked), {object});
    W32(g_scratch + kScratchNode, object);
    CallGuest(ctx, kPushBack, {tool + kToolSelection, g_scratch + kScratchNode});
    CallGuest(ctx, kSelectionChanged, {tool});
}

}  // namespace fcip::editor
