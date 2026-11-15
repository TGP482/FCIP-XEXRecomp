// ige.xex, the solo level editor: a gizmo on the picked entity or mission object. An entity follows the drag live
// through its editor object (as the editor's own undo moves it, 8210FD98: SetPos +0x10, SetAngles +0x18); a mission
// object is shown where it would go and moved in the kept mission on release. Each drag is one step of undo.
#include "leveleditor/gizmo.h"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/guest.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/object_panel.h"
#include "leveleditor/picking.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr float kSizeOfDistance = 0.12f;  // the arrows' length, of the camera's distance
constexpr float kRingShare = 0.8f;        // a ring's radius, of the arrows' length
constexpr float kGrabPixels = 7.0f;
constexpr int kRingSegments = 48;
constexpr float kPi = 3.14159265f;
constexpr float kDegrees = 180.0f / kPi;
constexpr ImU32 kAxisColours[3] = {IM_COL32(230, 70, 70, 255), IM_COL32(80, 210, 80, 255),
                                   IM_COL32(80, 130, 240, 255)};
constexpr ImU32 kActiveColour = IM_COL32(255, 220, 60, 255);
constexpr ImU32 kGhostColour = IM_COL32(255, 255, 255, 160);

// editor object methods: Get (out, this) returns out, Set (this, Vec3)
constexpr uint32_t kObjectGetPos = 0x0C;
constexpr uint32_t kObjectSetPos = 0x10;
constexpr uint32_t kObjectGetAngles = 0x14;
constexpr uint32_t kObjectSetAngles = 0x18;
constexpr uint32_t kScratchOut = 0x00;
constexpr uint32_t kScratchIn = 0x0C;
constexpr uint32_t kScratchBytes = 0x18;

using Vec = std::array<float, 3>;

enum Handle : int { kNone = -1, kMoveX, kMoveY, kMoveZ, kTurnX, kTurnY, kTurnZ };

struct Target {
    uint32_t entity_id = 0;
    std::string name;
    uint32_t object = 0;  // a mission object
    Vec pos{};
    std::vector<Vec> points;  // a mission object's, which turns about z alone
};

struct Drag {
    Handle handle = kNone;
    Target target;
    ImVec2 grab;
    ImVec2 axis_dir;            // a move: the axis on screen, normalised
    float pixels_per_metre = 0.0f;
    ImVec2 centre;              // a turn: the gizmo on screen
    float last_angle = 0.0f;
    float facing = 1.0f;        // the axis points at the camera
    Vec move{};
    Vec turn{};                 // degrees
};
Drag g_drag;

// a drag's latest place for the editor thread
struct Live {
    uint32_t entity_id = 0;
    std::string name;
    Vec move{};
    Vec turn{};
    bool first = false;
    bool commit = false;
};
std::mutex g_lock;
Live g_live;
bool g_live_pending = false;

// editor thread
uint32_t g_scratch = 0;
Vec g_start_pos{};
Vec g_start_angles{};

Vec Add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

// the end of the arrow along axis
Vec Tip(const Vec& p, int axis, float length) {
    Vec tip = p;
    tip[size_t(axis)] += length;
    return tip;
}

bool FindTarget(Target& t) {
    if (uint32_t object = SelectedAiObject()) {
        for (const AiObject& o : ReadAiObjects()->objects) {
            if (o.node != object || !o.placed) continue;
            t.object = object;
            t.pos = {o.pos[0], o.pos[1], o.pos[2]};
            t.points = o.points;
            return true;
        }
        return false;
    }
    uint32_t id = SelectedEntityId();
    for (const ViewEntity& e : *ViewEntities()) {
        if (!id || e.id != id) continue;
        t.entity_id = id;
        t.name = e.name;
        t.pos = {e.pos[0], e.pos[1], e.pos[2]};
        return true;
    }
    return false;
}

float Length(const View& v, const Vec& p) {
    if (!v.has_eye) return 2.0f;
    return std::hypot(p[0] - v.eye[0], p[1] - v.eye[1], p[2] - v.eye[2]) * kSizeOfDistance;
}

bool TurnsAbout(const Target& t, int axis) { return t.points.empty() || axis == 2; }

// the ring about axis as world points
std::vector<Vec> Ring(const Vec& p, int axis, float radius) {
    std::vector<Vec> ring;
    for (int s = 0; s <= kRingSegments; s++) {
        float a = 2.0f * kPi * float(s) / float(kRingSegments);
        // the two axes other than axis, in order
        int first = axis == 0 ? 1 : 0;
        int second = axis == 2 ? 1 : 2;
        Vec point = p;
        point[size_t(first)] += std::cos(a) * radius;
        point[size_t(second)] += std::sin(a) * radius;
        ring.push_back(point);
    }
    return ring;
}

float SegmentDistance(ImVec2 m, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = dx * dx + dy * dy;
    float t = length > 0.0f ? ((m.x - a.x) * dx + (m.y - a.y) * dy) / length : 0.0f;
    t = std::fmax(0.0f, std::fmin(1.0f, t));
    return std::hypot(m.x - (a.x + dx * t), m.y - (a.y + dy * t));
}

float PolylineDistance(const View& v, ImVec2 m, const std::vector<Vec>& points) {
    float best = 1e9f;
    for (size_t i = 1; i < points.size(); i++) {
        ImVec2 a;
        ImVec2 b;
        if (Project(v, points[i - 1].data(), a) && Project(v, points[i].data(), b)) {
            best = std::fmin(best, SegmentDistance(m, a, b));
        }
    }
    return best;
}

Handle HandleAt(const View& v, const Target& t, ImVec2 m) {
    float length = Length(v, t.pos);
    Handle best = kNone;
    float nearest = kGrabPixels;
    for (int i = 0; i < 3; i++) {
        float d = PolylineDistance(v, m, {t.pos, Tip(t.pos, i, length)});
        if (d < nearest) {
            nearest = d;
            best = Handle(kMoveX + i);
        }
        if (!TurnsAbout(t, i)) continue;
        d = PolylineDistance(v, m, Ring(t.pos, i, length * kRingShare));
        if (d < nearest) {
            nearest = d;
            best = Handle(kTurnX + i);
        }
    }
    return best;
}

void SendLive(bool first, bool commit) {
    std::lock_guard lock(g_lock);
    first |= g_live_pending && g_live.first;
    g_live = {g_drag.target.entity_id, g_drag.target.name, g_drag.move, g_drag.turn, first, commit};
    g_live_pending = true;
}

void Grab(const View& v, Handle handle, const Target& t, ImVec2 m) {
    g_drag = {};
    g_drag.handle = handle;
    g_drag.target = t;
    g_drag.grab = m;
    float length = Length(v, t.pos);
    ImVec2 centre;
    Project(v, t.pos.data(), centre);
    g_drag.centre = centre;
    int axis = handle >= kTurnX ? handle - kTurnX : handle - kMoveX;
    if (handle <= kMoveZ) {
        Vec tip = Tip(t.pos, axis, length);
        ImVec2 end;
        Project(v, tip.data(), end);
        float dx = end.x - centre.x;
        float dy = end.y - centre.y;
        float pixels = std::hypot(dx, dy);
        g_drag.axis_dir = pixels > 0.0f ? ImVec2(dx / pixels, dy / pixels) : ImVec2(0, 0);
        g_drag.pixels_per_metre = pixels / length;
    } else {
        g_drag.last_angle = std::atan2(m.y - centre.y, m.x - centre.x);
        float toward = v.eye[size_t(axis)] - t.pos[size_t(axis)];
        g_drag.facing = toward >= 0.0f ? 1.0f : -1.0f;
    }
    if (t.entity_id) SendLive(true, false);
}

void Follow(ImVec2 m) {
    if (g_drag.handle <= kMoveZ) {
        if (g_drag.pixels_per_metre <= 0.0f) return;
        float along = (m.x - g_drag.grab.x) * g_drag.axis_dir.x + (m.y - g_drag.grab.y) * g_drag.axis_dir.y;
        g_drag.move[size_t(g_drag.handle - kMoveX)] = along / g_drag.pixels_per_metre;
    } else {
        float angle = std::atan2(m.y - g_drag.centre.y, m.x - g_drag.centre.x);
        float delta = std::remainder(angle - g_drag.last_angle, 2.0f * kPi);
        g_drag.last_angle = angle;
        // screen y points down, so a turn the camera sees anticlockwise reads negative
        g_drag.turn[size_t(g_drag.handle - kTurnX)] -= delta * kDegrees * g_drag.facing;
    }
    if (g_drag.target.entity_id) SendLive(false, false);
}

void Release() {
    const Target& t = g_drag.target;
    if (t.entity_id) SendLive(false, true);
    if (t.object && (g_drag.move != Vec{} || g_drag.turn != Vec{})) PlaceAiObject(t.object, g_drag.move, g_drag.turn);
    g_drag.handle = kNone;
}

// a point of the mission object where the drag would put it
Vec Placed(const Target& t, const Vec& p) {
    float radians = g_drag.turn[2] / kDegrees;
    float x = p[0] - t.pos[0];
    float y = p[1] - t.pos[1];
    float c = std::cos(radians);
    float s = std::sin(radians);
    Vec turned = {x * c - y * s, x * s + y * c, p[2] - t.pos[2]};
    return Add(Add(t.pos, turned), g_drag.move);
}

void DrawGhost(const View& v, const Target& t) {
    Vec pos = Add(t.pos, g_drag.move);
    Square(v, pos.data(), kGhostColour);
    if (t.points.empty()) return;
    std::vector<std::array<float, 3>> placed;
    for (const Vec& p : t.points) placed.push_back(Placed(t, p));
    Prism(v, placed, 0.0f, true, kGhostColour);
}

// Editor thread

Vec ReadVec(uint32_t at) { return {GF(at), GF(at + 4), GF(at + 8)}; }

void WriteVec(uint32_t at, const Vec& v) {
    for (uint32_t i = 0; i < 3; i++) {
        float f = v[i];
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        W32(at + i * 4, bits);
    }
}

Vec GetVec(PPCContext& ctx, uint32_t object, uint32_t method) {
    uint32_t out = uint32_t(CallGuest(ctx, Method(object, method), {g_scratch + kScratchOut, object}));
    return ReadVec(out ? out : g_scratch + kScratchOut);
}

void SetVec(PPCContext& ctx, uint32_t object, uint32_t method, const Vec& v) {
    WriteVec(g_scratch + kScratchIn, v);
    CallGuest(ctx, Method(object, method), {object, g_scratch + kScratchIn});
}

void Place(PPCContext& ctx, uint32_t entity_id, const Vec& pos, const Vec& angles) {
    uint32_t object = EditorObjectOfEntity(ctx, entity_id);
    if (!object) return;
    SetVec(ctx, object, kObjectSetPos, pos);
    SetVec(ctx, object, kObjectSetAngles, angles);
}

}  // namespace

namespace fcip::editor {

bool GizmoMouse(ImVec2 mouse, bool pressed, bool down) {
    if (g_drag.handle != kNone) {
        if (down) Follow(mouse);
        else Release();
        return true;
    }
    View v;
    Target t;
    if (!pressed || !ReadView(v) || !FindTarget(t)) return false;
    Handle handle = HandleAt(v, t, mouse);
    if (handle == kNone) return false;
    Grab(v, handle, t, mouse);
    return true;
}

void DrawGizmo(const View& v) {
    if (Read().tool != kEditObject) return;
    Target t;
    bool dragging = g_drag.handle != kNone;
    if (dragging) t = g_drag.target;
    else if (!FindTarget(t)) return;
    if (dragging && t.object) DrawGhost(v, t);
    Vec pos = dragging ? Add(t.pos, g_drag.move) : t.pos;
    Handle hover = dragging ? g_drag.handle : HandleAt(v, t, ImGui::GetIO().MousePos);
    float length = Length(v, t.pos);
    for (int i = 0; i < 3; i++) {
        Vec tip = Tip(pos, i, length);
        Arrow(v, pos.data(), tip.data(), hover == kMoveX + i ? kActiveColour : kAxisColours[i]);
        if (!TurnsAbout(t, i)) continue;
        std::vector<Vec> ring = Ring(pos, i, length * kRingShare);
        ImU32 colour = hover == kTurnX + i ? kActiveColour : kAxisColours[i];
        for (size_t s = 1; s < ring.size(); s++) Line(v, ring[s - 1].data(), ring[s].data(), colour);
    }
}

void ApplyGizmo(PPCContext& ctx) {
    Live live;
    {
        std::lock_guard lock(g_lock);
        if (!g_live_pending) return;
        live = g_live;
        g_live_pending = false;
    }
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    uint32_t object = g_scratch ? EditorObjectOfEntity(ctx, live.entity_id) : 0;
    if (!object) return;
    if (live.first) {
        g_start_pos = GetVec(ctx, object, kObjectGetPos);
        g_start_angles = GetVec(ctx, object, kObjectGetAngles);
    }
    Place(ctx, live.entity_id, Add(g_start_pos, live.move), Add(g_start_angles, live.turn));
    if (!live.commit || (live.move == Vec{} && live.turn == Vec{})) return;
    NoteMapChanged();
    uint32_t id = live.entity_id;
    Vec pos = g_start_pos;
    Vec angles = g_start_angles;
    PushUndo(ctx, "Move " + live.name, [id, pos, angles](PPCContext& ctx) { Place(ctx, id, pos, angles); });
}

}  // namespace fcip::editor
