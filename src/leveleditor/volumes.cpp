// ige.xex, the solo level editor: what the picked object's volume is made of, dragged in the view. A box entity's
// Dim values (SetLocalBBox(-Dim/2, Dim/2) in its script), the radii a sound, light, alarm, objective or spawn spot
// reads, an AreaBox's Width, Length and Height, an AreaSphere's Radius and an area's points and Height
// (area_markers.cpp has how the engine reads them). A point drags on its own height, a mid point handle adds a point
// there, Alt and a click removes one. Each drag is one step of undo.
#include "leveleditor/volumes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "editor/tools.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

using Vec = std::array<float, 3>;

constexpr float kGrabPixels = 8.0f;
constexpr float kHandleHalf = 4.0f;  // pixels
constexpr float kMinSize = 0.1f;     // metres
constexpr size_t kMinPoints = 2;
constexpr ImU32 kHandleColour = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kInsertColour = IM_COL32(255, 255, 255, 120);
constexpr ImU32 kActiveColour = IM_COL32(255, 220, 60, 255);

// an entity class whose script sizes its box from its Properties: three values, or one "x,y,z" value
struct BoxKeys {
    const char* cls;
    const char* keys[3];
    const char* vector;
};
constexpr BoxKeys kBoxKeys[] = {
    {"ProximityTrigger", {"DimX", "DimY", "DimZ"}, nullptr},
    {"ProximityTrigger_MP", {"DimX", "DimY", "DimZ"}, nullptr},
    {"EndMapTrigger", {"DimX", "DimY", "DimZ"}, nullptr},
    {"Checkpoint", {"DimX", "DimY", "DimZ"}, nullptr},
    {"UtilityTrigger", {"DimX", "DimY", "DimZ"}, nullptr},
    {"EnvironmentTrigger", {"DimX", "DimY", "DimZ"}, nullptr},
    {"PlaceableExplo", {"DimX", "DimY", "DimZ"}, nullptr},
    {"WaterCurrent", {}, "vector_Dim"},
    {"ProximityDamage", {}, "vector_BBoxDim"},
    {"AutomaticDoor", {"BBOX_Size/X", "BBOX_Size/Y", "BBOX_Size/Z"}, nullptr},
    {"AutomaticDoor1Piece", {"BBOX_Size/X", "BBOX_Size/Y", "BBOX_Size/Z"}, nullptr},
};

// an entity class whose script reaches as far as one of its Properties
struct RadiusKey {
    const char* cls;
    const char* key;
};
// the radii their scripts read (a SoundSpot's roll offs of -1 keep the sound's own, so they get no handle)
constexpr RadiusKey kRadiusKeys[] = {
    {"CreatureGeneratorSpawnSpot", "fSpawnRadius"}, {"SoundSpot", "fRollOff1_Saturation"},
    {"SoundSpot", "fRollOff2_Background"},          {"SoundSpot", "fRollOff3_Transition"},
    {"SoundSpot", "fRollOff4_Inaudible"},           {"ProximitySoundSpot", "OuterRadius"},
    {"DynamicLight", "AttenuationRadius"},          {"SoundSupressor", "fRadius"},
    {"SonicAlarm", "DamageRadius"},                 {"Waypoint", "Radius"},
    {"DamagingAnimation", "Radius"},
};
constexpr float kRadiusTurn = 3.14159265f / 4.0f;  // each radius's handle an eighth of a turn from the last

enum class Shape { None, EntityBox, EntityRadius, AreaBox, Sphere, Points };

// the picked object's volume as the snapshots have it
struct Volume {
    Shape shape = Shape::None;
    uint32_t entity_id = 0;
    const BoxKeys* keys = nullptr;
    std::vector<const char*> radius_keys;
    std::vector<float> radii;
    uint32_t object = 0;
    Vec centre{};
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec size{};  // full sizes: an entity box's sides, an AreaBox's Width, Length, Height, a sphere's Radius first
    std::vector<Vec> points;
    float bottom = 0.0f;
};

enum class Kind { Size, Point, Insert };

// a handle drags along dir from base (sizes), or on a level plane (points)
struct Handle {
    Kind kind = Kind::Size;
    int axis = 0;      // the size it sets
    size_t index = 0;  // the point, or where an inserted one goes
    Vec at{};
    Vec base{};
    Vec dir{};
    float scale = 1.0f;  // a size of a centred box is twice its handle's distance
};

struct Drag {
    bool on = false;
    Volume volume;
    Handle handle;
    float value = 0.0f;
    Vec point{};
};
Drag g_drag;

Vec Add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec Scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec AxisOf(const Volume& v, int i) { return {v.axes[i][0], v.axes[i][1], v.axes[i][2]}; }

Vec ParseVec(const std::string& text) {
    Vec v{};
    std::sscanf(text.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
    return v;
}

std::string Format(float f) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", f);
    return text;
}

const BoxKeys* KeysOf(const std::string& cls) {
    for (const BoxKeys& k : kBoxKeys) {
        if (cls == k.cls) return &k;
    }
    return nullptr;
}

bool RadiusVolume(const ViewEntity& e, Volume& out) {
    for (const RadiusKey& k : kRadiusKeys) {
        float radius = std::strtof(PropertyOf(e, k.key).c_str(), nullptr);
        if (e.cls != k.cls || radius < 0.0f) continue;
        out.radius_keys.push_back(k.key);
        out.radii.push_back(radius);
    }
    if (out.radii.empty()) return false;
    out.shape = Shape::EntityRadius;
    out.entity_id = e.id;
    out.centre = {e.pos[0], e.pos[1], e.pos[2]};
    return true;
}

bool EntityVolume(Volume& out) {
    uint32_t id = SelectedEntityId();
    for (const ViewEntity& e : *ViewEntities()) {
        if (!id || e.id != id) continue;
        if (e.properties.empty()) return false;
        if (RadiusVolume(e, out)) return true;
        const BoxKeys* keys = KeysOf(e.cls);
        if (!keys) return false;
        out.shape = Shape::EntityBox;
        out.entity_id = id;
        out.keys = keys;
        if (keys->vector) out.size = ParseVec(PropertyOf(e, keys->vector));
        for (int i = 0; i < 3 && !keys->vector; i++) {
            out.size[size_t(i)] = std::strtof(PropertyOf(e, keys->keys[i]).c_str(), nullptr);
        }
        Vec pos = {e.pos[0], e.pos[1], e.pos[2]};
        Vec box = {(e.box_min[0] + e.box_max[0]) * 0.5f, (e.box_min[1] + e.box_max[1]) * 0.5f,
                   (e.box_min[2] + e.box_max[2]) * 0.5f};
        out.centre = e.has_box ? box : pos;
        AxesFromAngles(e.angles, out.axes);
        return true;
    }
    return false;
}

float FloatOf(const AiObject& o, const char* key) { return std::strtof(AttributeOf(o, key).c_str(), nullptr); }

bool ObjectVolume(Volume& out) {
    uint32_t picked = SelectedAiObject();
    for (const AiObject& o : ReadAiObjects()->objects) {
        if (!picked || o.node != picked) continue;
        out.object = o.node;
        out.centre = {o.pos[0], o.pos[1], o.pos[2]};
        if (o.type == "AreaBox") {
            out.shape = Shape::AreaBox;
            out.size = {FloatOf(o, "Width"), FloatOf(o, "Length"), FloatOf(o, "Height")};
            Vec angles = ParseVec(AttributeOf(o, "Angles"));
            AxesFromAngles(angles.data(), out.axes);
        } else if (o.type == "AreaSphere") {
            out.shape = Shape::Sphere;
            out.size[0] = FloatOf(o, "Radius");
        } else if (!o.points.empty()) {
            out.shape = Shape::Points;
            out.points = o.points;
            out.size[2] = FloatOf(o, "Height");
            out.bottom = o.points.front()[2];
            for (const Vec& p : o.points) out.bottom = std::min(out.bottom, p[2]);
        }
        return out.shape != Shape::None;
    }
    return false;
}

bool PickedVolume(Volume& out) { return ObjectVolume(out) || EntityVolume(out); }

Vec Centroid(const std::vector<Vec>& points) {
    Vec sum{};
    for (const Vec& p : points) sum = Add(sum, p);
    return Scale(sum, 1.0f / float(points.size()));
}

Handle SizeHandle(const Volume& v, int axis, Vec base, float distance, float scale) {
    Handle h;
    h.axis = axis;
    h.base = base;
    h.dir = AxisOf(v, axis);
    h.at = Add(base, Scale(h.dir, distance));
    h.scale = scale;
    return h;
}

std::vector<Handle> HandlesOf(const Volume& v) {
    std::vector<Handle> handles;
    switch (v.shape) {
        case Shape::EntityBox:
            for (int i = 0; i < 3; i++) {
                handles.push_back(SizeHandle(v, i, v.centre, v.size[size_t(i)] * 0.5f, 2.0f));
            }
            break;
        case Shape::AreaBox: {
            // centred in x and y, Height up from Pos
            Vec middle = Add(v.centre, Scale(AxisOf(v, 2), v.size[2] * 0.5f));
            handles.push_back(SizeHandle(v, 0, middle, v.size[0] * 0.5f, 2.0f));
            handles.push_back(SizeHandle(v, 1, middle, v.size[1] * 0.5f, 2.0f));
            handles.push_back(SizeHandle(v, 2, v.centre, v.size[2], 1.0f));
            break;
        }
        case Shape::Sphere: handles.push_back(SizeHandle(v, 0, v.centre, v.size[0], 1.0f)); break;
        case Shape::EntityRadius:
            for (size_t i = 0; i < v.radii.size(); i++) {
                Handle h = SizeHandle(v, 0, v.centre, 0.0f, 1.0f);
                h.index = i;
                h.dir = {std::cos(kRadiusTurn * float(i)), std::sin(kRadiusTurn * float(i)), 0.0f};
                h.at = Add(v.centre, Scale(h.dir, v.radii[i]));
                handles.push_back(h);
            }
            break;
        case Shape::Points: {
            Vec middle = Centroid(v.points);
            middle[2] = v.bottom;
            handles.push_back(SizeHandle(v, 2, middle, v.size[2], 1.0f));
            for (size_t i = 0; i < v.points.size(); i++) {
                Handle point;
                point.kind = Kind::Point;
                point.index = i;
                point.at = v.points[i];
                handles.push_back(point);
                Handle insert = point;
                insert.kind = Kind::Insert;
                insert.index = i + 1;
                insert.at = Scale(Add(v.points[i], v.points[(i + 1) % v.points.size()]), 0.5f);
                handles.push_back(insert);
            }
            break;
        }
        case Shape::None: break;
    }
    return handles;
}

float PixelsTo(const View& view, ImVec2 m, const Vec& p) {
    ImVec2 at;
    if (!Project(view, p.data(), at)) return 1e9f;
    return std::hypot(at.x - m.x, at.y - m.y);
}

int HandleAt(const View& view, const std::vector<Handle>& handles, ImVec2 m) {
    int best = -1;
    float nearest = kGrabPixels;
    for (size_t i = 0; i < handles.size(); i++) {
        float d = PixelsTo(view, m, handles[i].at);
        if (d < nearest) {
            nearest = d;
            best = int(i);
        }
    }
    return best;
}

// the distance along the handle's line nearest the mouse's ray
bool AlongLine(const View& view, ImVec2 m, const Handle& h, float& out) {
    Vec o{};
    Vec d{};
    if (!MouseRay(view, m, o.data(), d.data())) return false;
    Vec w = Add(h.base, Scale(o, -1.0f));
    float b = Dot(h.dir, d);
    float denominator = 1.0f - b * b;
    if (std::fabs(denominator) < 1e-4f) return false;
    out = (b * Dot(d, w) - Dot(h.dir, w)) / denominator;
    return true;
}

// where the mouse's ray meets the level plane at height z
bool OnPlane(const View& view, ImVec2 m, float z, Vec& out) {
    Vec o{};
    Vec d{};
    if (!MouseRay(view, m, o.data(), d.data()) || std::fabs(d[2]) < 1e-4f) return false;
    float t = (z - o[2]) / d[2];
    if (t <= 0.0f) return false;
    out = Add(o, Scale(d, t));
    out[2] = z;
    return true;
}

void Follow(const View& view, ImVec2 m) {
    const Handle& h = g_drag.handle;
    if (h.kind == Kind::Size) {
        float along = 0.0f;
        if (AlongLine(view, m, h, along)) g_drag.value = std::max(kMinSize, along * h.scale);
        return;
    }
    const std::vector<Vec>& points = g_drag.volume.points;
    float z = h.kind == Kind::Point ? points[h.index][2] : points[h.index - 1][2];
    Vec p{};
    if (OnPlane(view, m, z, p)) g_drag.point = p;
}

// the volume as the drag leaves it
std::vector<Vec> DraggedPoints() {
    std::vector<Vec> points = g_drag.volume.points;
    const Handle& h = g_drag.handle;
    if (h.kind == Kind::Point) points[h.index] = g_drag.point;
    if (h.kind == Kind::Insert) points.insert(points.begin() + std::ptrdiff_t(h.index), g_drag.point);
    return points;
}

Vec DraggedSize() {
    Vec size = g_drag.volume.size;
    if (g_drag.handle.kind == Kind::Size) size[size_t(g_drag.handle.axis)] = g_drag.value;
    return size;
}

void CommitEntity(const Volume& v, const Vec& size) {
    const BoxKeys& k = *v.keys;
    int axis = g_drag.handle.axis;
    if (!k.vector) {
        SetEntityProperty(v.entity_id, k.keys[axis], Format(size[size_t(axis)]));
        return;
    }
    std::string text = Format(size[0]) + "," + Format(size[1]) + "," + Format(size[2]);
    SetEntityProperty(v.entity_id, k.vector, text);
}

void Commit() {
    const Volume& v = g_drag.volume;
    Vec size = DraggedSize();
    constexpr const char* kAreaBoxKeys[] = {"Width", "Length", "Height"};
    int axis = g_drag.handle.axis;
    switch (v.shape) {
        case Shape::EntityBox: CommitEntity(v, size); break;
        case Shape::AreaBox: SetAiAttribute(v.object, {}, kAreaBoxKeys[axis], Format(size[size_t(axis)])); break;
        case Shape::Sphere: SetAiAttribute(v.object, {}, "Radius", Format(size[0])); break;
        case Shape::EntityRadius:
            SetEntityProperty(v.entity_id, v.radius_keys[g_drag.handle.index], Format(g_drag.value));
            break;
        case Shape::Points:
            if (g_drag.handle.kind == Kind::Size) SetAiAttribute(v.object, {}, "Height", Format(size[2]));
            else SetAiPoints(v.object, DraggedPoints());
            break;
        case Shape::None: break;
    }
}

bool RemovePoint(const Volume& v, const Handle& h) {
    if (h.kind != Kind::Point || v.points.size() <= kMinPoints) return false;
    std::vector<Vec> points = v.points;
    points.erase(points.begin() + std::ptrdiff_t(h.index));
    SetAiPoints(v.object, points);
    return true;
}

void DrawHandle(const View& view, const Vec& p, ImU32 colour) {
    ImVec2 at;
    if (!Project(view, p.data(), at)) return;
    ImVec2 min(at.x - kHandleHalf, at.y - kHandleHalf);
    ImVec2 max(at.x + kHandleHalf, at.y + kHandleHalf);
    view.draw->AddRectFilled(min, max, colour);
}

void DrawDragged(const View& view) {
    const Volume& v = g_drag.volume;
    if (v.shape == Shape::Points) {
        Prism(view, DraggedPoints(), 0.0f, true, kActiveColour);
        return;
    }
    Handle h = g_drag.handle;
    float distance = g_drag.value / h.scale;
    if (v.shape == Shape::EntityRadius) Circle(view, h.base.data(), distance, kActiveColour);
    Line(view, h.base.data(), Add(h.base, Scale(h.dir, distance)).data(), kActiveColour);
    DrawHandle(view, Add(h.base, Scale(h.dir, distance)), kActiveColour);
}

}  // namespace

namespace fcip::editor {

bool VolumeMouse(ImVec2 mouse, bool pressed, bool down) {
    if (g_drag.on) {
        View view;
        if (down && ReadView(view)) Follow(view, mouse);
        if (!down) {
            Commit();
            g_drag.on = false;
        }
        return true;
    }
    View view;
    Volume v;
    if (!pressed || !ReadView(view) || !PickedVolume(v)) return false;
    std::vector<Handle> handles = HandlesOf(v);
    int at = HandleAt(view, handles, mouse);
    if (at < 0) return false;
    const Handle& h = handles[size_t(at)];
    if (ImGui::GetIO().KeyAlt) return RemovePoint(v, h);
    float value = v.shape == Shape::EntityRadius ? v.radii[h.index] : v.size[size_t(h.axis)];
    g_drag = {true, v, h, value, h.at};
    return true;
}

void DrawVolumeHandles(const View& view) {
    if (Read().tool != kEditObject) return;
    if (g_drag.on) {
        DrawDragged(view);
        return;
    }
    Volume v;
    if (!PickedVolume(v)) return;
    for (const Handle& h : HandlesOf(v)) {
        DrawHandle(view, h.at, h.kind == Kind::Insert ? kInsertColour : kHandleColour);
    }
    for (float radius : v.radii) Circle(view, v.centre.data(), radius, kHandleColour);
}

}  // namespace fcip::editor
