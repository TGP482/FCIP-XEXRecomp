// ige.xex, the solo level editor: a campaign level's mission XML and LevelData.xml stay referenced after it loads. The
// mission's Objects hold the AI objects (tag points, anchors with their actions, shapes, areas), which the engine reads
// into the AI system only as the level starts (8229CD10). The panel edits them here, and the custom level save writes
// the mission back with the editor's entities in place of the campaign's.
#include "leveleditor/mission.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "kernel/memory.h"
#include "editor/tools.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/guest.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kObjects = "Objects";
constexpr const char* kObject = "Object";
constexpr const char* kEntity = "Entity";
constexpr const char* kPoints = "Points";
constexpr const char* kEntities = "Entities";

// what a new object gets after Type, Name and Pos, the campaign's missions' most common values
struct Default {
    const char* key;
    const char* value;
};
constexpr Default kTagPointDefaults[] = {{"Angles", "0,0,0"}, {"Action", ""}};
constexpr Default kAnchorDefaults[] = {
    {"Angles", "0,0,0"}, {"Action", "AIANCHOR_GENERIC_JOB"}, {"Enabled", "1"}, {"AutoDisable", "0"},
    {"GroupID", "-1"},   {"Range", "-1"},                    {"MaxUsers", "1"}, {"NextAnchor", ""},
    {"AnchorId", "9"},   {"Area", "0"},
};
constexpr Default kAreaDefaults[] = {
    {"Width", "0"},  {"AreaId", "0"},               {"GroupId", "0"},          {"Closed", "1"},
    {"Indoor", "0"}, {"SoundOcclusionFactor", "0"}, {"AudibilityAreaID", "0"}, {"ActivateWithBino", "0"},
};
constexpr Default kVisAreaDefaults[] = {
    {"AmbientColor", "0,0,0"}, {"AffectedBySun", "0"}, {"SkyOnly", "0"},       {"WaterVisible", "0"},
    {"UseGlowParams", "0"},    {"GlowBrightness", "0"}, {"GlowIntensity", "0"}, {"GlowBias", "0"},
    {"LoadingDepth", "4"},
};
constexpr Default kPathDefaults[] = {{"Height", "0"}, {"Closed", "0"}, {"AI_Drift", "0"}, {"Drift", "0"}};
constexpr Default kAreaBoxDefaults[] = {
    {"AreaId", "-1"}, {"FadeInZone", "0"}, {"Width", "4"},  {"Length", "4"},
    {"Height", "8"},  {"GroupId", "0"},    {"Indoor", "0"}, {"SoundOcclusionFactor", "0"},
    {"AudibilityAreaID", "0"}, {"ActivateWithBino", "0"},
};
constexpr Default kAreaSphereDefaults[] = {
    {"AreaId", "-1"}, {"FadeInZone", "0"}, {"Radius", "10"}, {"GroupId", "0"},
    {"Indoor", "0"},  {"SoundOcclusionFactor", "0"},         {"AudibilityAreaID", "0"}, {"ActivateWithBino", "0"},
};

// a new object's attributes in order, and whether it gets a square of points from its Pos
struct NewObject {
    const char* type;
    std::span<const Default> first;
    std::span<const Default> then;
    const char* height;  // nullptr for none
    bool points;
};
constexpr NewObject kNewObjects[] = {
    {"Respawn", kTagPointDefaults, {}, nullptr, false},
    {"TagPoint", kTagPointDefaults, {}, nullptr, false},
    {"AIAnchor", kAnchorDefaults, {}, nullptr, false},
    {"AreaBox", kAreaBoxDefaults, {}, nullptr, false},
    {"AreaSphere", kAreaSphereDefaults, {}, nullptr, false},
    {"Shape", kAreaDefaults, {}, "4", true},
    {"VisArea", kAreaDefaults, kVisAreaDefaults, "8", true},
    {"Portal", kAreaDefaults, kVisAreaDefaults, "5", true},
    {"AIPath", kAreaDefaults, kPathDefaults, nullptr, false},
    {"ForbiddenArea", kAreaDefaults, {}, "0", true},
    {"EnclosingArea", kAreaDefaults, {}, "0", true},
    {"FenceArea", kAreaDefaults, {}, "100", true},
    {"AINavigationModifier", kAreaDefaults, {}, "0", true},
};
constexpr float kSquareMetres = 4.0f;  // a new area's side

// guest scratch: a node ref, then the texts of a call
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchKey = 0x10;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = kScratchKey + kScratchKeyBytes;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchValue + kScratchValueBytes;

using Edit = std::function<void(PPCContext&)>;

std::mutex g_lock;
std::shared_ptr<const AiObjects> g_snapshot = std::make_shared<AiObjects>();
std::shared_ptr<const AiObjects> g_loaded = std::make_shared<AiObjects>();
std::vector<Edit> g_edits;
// editor thread
uint32_t g_mission = 0;
uint32_t g_level_data = 0;
bool g_stale = false;
bool g_first_refresh = false;  // the next snapshot is the loaded one
uint32_t g_scratch = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

std::vector<XmlAttribute> Attributes(uint32_t node) {
    std::vector<XmlAttribute> out;
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        out.push_back({GuestString(a + kAttrKey), GuestString(a + kAttrValue)});
    }
    return out;
}

std::string Find(const std::vector<XmlAttribute>& attributes, const char* key) {
    for (const XmlAttribute& a : attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

bool ParseVec3(const std::string& text, float out[3]) {
    return std::sscanf(text.c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

std::string FormatVec3(const std::array<float, 3>& v) {
    char text[64];
    std::snprintf(text, sizeof(text), "%g,%g,%g", v[0], v[1], v[2]);
    return text;
}

uint32_t ChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        if (Tag(ChildAt(node, i)) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t ObjectsNode() { return g_mission ? ChildTagged(g_mission, kObjects) : 0; }

void Release(PPCContext& ctx, uint32_t& node) {
    if (node) CallGuest(ctx, Method(node, kNodeRelease), {node});
    node = 0;
}

void Keep(PPCContext& ctx, uint32_t& kept, uint32_t node) {
    Release(ctx, kept);
    if (node) CallGuest(ctx, Method(node, kNodeAddRef), {node});
    kept = node;
}

// an edit names an object the panel saw; it may have gone since
bool IsAiObject(uint32_t node) {
    uint32_t objects = ObjectsNode();
    if (!objects || !node) return false;
    for (uint32_t i = 0; i < ChildCount(objects); i++) {
        if (ChildAt(objects, i) == node) return Tag(node) == kObject;
    }
    return false;
}

void ReadElements(uint32_t node, const XmlElement& at, std::vector<XmlElement>& out) {
    XmlElement e = at;
    e.tag = Tag(node);
    e.attributes = Attributes(node);
    out.push_back(e);
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        XmlElement child{at.depth + 1, at.path, {}, {}};
        child.path.push_back(int(i));
        ReadElements(ChildAt(node, i), child, out);
    }
}

AiObject ReadObject(uint32_t node) {
    AiObject o;
    o.node = node;
    ReadElements(node, XmlElement{}, o.elements);
    const std::vector<XmlAttribute>& attributes = o.elements.front().attributes;
    o.type = Find(attributes, "Type");
    o.name = Find(attributes, "Name");
    o.placed = ParseVec3(Find(attributes, "Pos"), o.pos);
    uint32_t points = ChildTagged(node, kPoints);
    for (uint32_t i = 0; points && i < ChildCount(points); i++) {
        std::array<float, 3> p{};
        if (ParseVec3(Find(Attributes(ChildAt(points, i)), "Pos"), p.data())) o.points.push_back(p);
    }
    uint32_t entities = ChildTagged(node, kEntities);
    for (uint32_t i = 0; entities && i < ChildCount(entities); i++) {
        o.entities.push_back(Find(Attributes(ChildAt(entities, i)), "Name"));
    }
    return o;
}

void Publish(AiObjects next) {
    std::lock_guard lock(g_lock);
    next.generation = g_snapshot->generation + 1;
    g_snapshot = std::make_shared<AiObjects>(std::move(next));
}

void Refresh() {
    AiObjects next;
    next.kept = g_mission != 0;
    uint32_t parent = ObjectsNode();
    for (uint32_t i = 0; parent && i < ChildCount(parent); i++) {
        uint32_t node = ChildAt(parent, i);
        std::string tag = Tag(node);
        if (tag == kObject) next.objects.push_back(ReadObject(node));
        if (tag != kEntity) continue;
        std::vector<XmlAttribute> attributes = Attributes(node);
        std::string layer = Find(attributes, "Layer");
        uint32_t id = uint32_t(std::strtoul(Find(attributes, "EntityId").c_str(), nullptr, 10));
        if (!layer.empty()) next.layers[id] = layer;
    }
    Publish(std::move(next));
    if (!g_first_refresh) return;
    g_first_refresh = false;
    std::lock_guard lock(g_lock);
    g_loaded = g_snapshot;
}

void SetAttr(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

// a new child of parent, released here: the parent keeps it
uint32_t NewChild(PPCContext& ctx, uint32_t parent, const std::string& tag) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(parent, kNodeNewChild), {g_scratch + kScratchRef, parent, KeyText(tag)});
    uint32_t child = R32(g_scratch + kScratchRef);
    if (child) CallGuest(ctx, Method(child, kNodeRelease), {child});
    return child;
}

void RemoveChild(PPCContext& ctx, uint32_t parent, uint32_t child) {
    CallGuest(ctx, Method(child, kNodeAddRef), {child});
    W32(g_scratch + kScratchRef, child);
    CallGuest(ctx, Method(parent, kNodeRemoveChild), {parent, g_scratch + kScratchRef});
}

// the mission's AI objects, referenced once each, and its Objects node taken out
std::vector<uint32_t> TakeAiObjects(PPCContext& ctx, uint32_t mission) {
    std::vector<uint32_t> taken;
    uint32_t objects = ChildTagged(mission, kObjects);
    if (!objects) return taken;
    for (uint32_t i = 0; i < ChildCount(objects); i++) {
        uint32_t node = ChildAt(objects, i);
        if (Tag(node) != kObject) continue;
        CallGuest(ctx, Method(node, kNodeAddRef), {node});
        taken.push_back(node);
    }
    RemoveChild(ctx, mission, objects);
    return taken;
}

// the last child tagged so: the Objects the editor's entity writer adds after the mission's own
uint32_t LastChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = ChildCount(node); i > 0; i--) {
        if (Tag(ChildAt(node, i - 1)) == tag) return ChildAt(node, i - 1);
    }
    return 0;
}

std::string UniqueName(const std::string& type) {
    std::set<std::string> names;
    uint32_t objects = ObjectsNode();
    for (uint32_t i = 0; i < ChildCount(objects); i++) {
        names.insert(Find(Attributes(ChildAt(objects, i)), "Name"));
    }
    for (int n = 1;; n++) {
        std::string name = type + std::to_string(n);
        if (!names.count(name)) return name;
    }
}

void SetDefaults(PPCContext& ctx, uint32_t node, std::span<const Default> defaults) {
    for (const Default& d : defaults) SetAttr(ctx, node, d.key, d.value);
}

// a closed square from pos, as the campaign's areas start their points at Pos
void AddSquare(PPCContext& ctx, uint32_t node, const std::array<float, 3>& pos) {
    constexpr float kCorners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    uint32_t points = NewChild(ctx, node, kPoints);
    for (const auto& c : kCorners) {
        std::array<float, 3> p = {pos[0] + c[0] * kSquareMetres, pos[1] + c[1] * kSquareMetres, pos[2]};
        if (uint32_t point = points ? NewChild(ctx, points, "Point") : 0) SetAttr(ctx, point, "Pos", FormatVec3(p));
    }
}

uint32_t ApplyAddObject(PPCContext& ctx, const std::string& type, const std::array<float, 3>& pos) {
    const NewObject* kind = nullptr;
    for (const NewObject& k : kNewObjects) {
        if (type == k.type) kind = &k;
    }
    uint32_t objects = ObjectsNode();
    uint32_t node = kind && objects ? NewChild(ctx, objects, kObject) : 0;
    if (!node) return 0;
    SetAttr(ctx, node, "Type", type);
    SetAttr(ctx, node, "Name", UniqueName(type));
    SetAttr(ctx, node, "Pos", FormatVec3(pos));
    SetDefaults(ctx, node, kind->first);
    SetDefaults(ctx, node, kind->then);
    if (kind->height) SetAttr(ctx, node, "Height", kind->height);
    if (kind->points) AddSquare(ctx, node, pos);
    return node;
}

// swaps the child at path with its sibling delta away
void ApplyMove(uint32_t object, std::vector<int> path, int delta) {
    if (path.empty()) return;
    int index = path.back();
    path.pop_back();
    uint32_t parent = FindNode(object, path);
    int other = index + delta;
    if (!parent || index < 0 || other < 0 || uint32_t(std::max(index, other)) >= ChildCount(parent)) return;
    uint32_t first = R32(parent + kNodeChildFirst);
    uint32_t a = R32(first + uint32_t(index) * 4);
    W32(first + uint32_t(index) * 4, R32(first + uint32_t(other) * 4));
    W32(first + uint32_t(other) * 4, a);
}

void ApplyRemove(PPCContext& ctx, uint32_t object, std::vector<int> path) {
    if (path.empty()) return;
    uint32_t child = FindNode(object, path);
    path.pop_back();
    uint32_t parent = FindNode(object, path);
    if (child && parent) RemoveChild(ctx, parent, child);
}

// Undo

// a node's children and attributes traded with another's, so the node keeps its identity
void SwapContent(uint32_t a, uint32_t b) {
    constexpr uint32_t kNodeParent = 0x44;
    constexpr uint32_t kVectorWords[] = {0x4C, 0x50, 0x54, 0x5C, 0x60, 0x64};  // children, attributes: first last end
    for (uint32_t at : kVectorWords) {
        uint32_t word = R32(a + at);
        W32(a + at, R32(b + at));
        W32(b + at, word);
    }
    for (uint32_t node : {a, b}) {
        for (uint32_t i = 0; i < ChildCount(node); i++) W32(ChildAt(node, i) + kNodeParent, node);
    }
}

uint32_t Clone(PPCContext& ctx, uint32_t node) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(node, kNodeClone), {g_scratch + kScratchRef, node});
    return R32(g_scratch + kScratchRef);
}

std::string Label(const std::string& what, uint32_t object) {
    return what + " " + Find(Attributes(object), "Name");
}

// the edit runs only while its object is still one of the mission's; undo puts its content back
void QueueOn(uint32_t object, std::string what, std::function<void(PPCContext&)> edit) {
    Queue([object, what = std::move(what), edit = std::move(edit)](PPCContext& ctx) {
        if (!IsAiObject(object)) return;
        uint32_t before = Clone(ctx, object);
        edit(ctx);
        NoteMapChanged();
        if (!before) return;
        auto undo = [object, before](PPCContext& ctx) {
            uint32_t kept = before;
            SwapContent(object, kept);
            Release(ctx, kept);
            g_stale = true;
        };
        auto forget = [before](PPCContext& ctx) {
            uint32_t kept = before;
            Release(ctx, kept);
        };
        PushUndo(ctx, Label(what, object), undo, forget);
    });
}

// a node put back where it was among parent's children
void Reinsert(PPCContext& ctx, uint32_t parent, uint32_t node, uint32_t index) {
    W32(g_scratch + kScratchRef, node);
    CallGuest(ctx, Method(parent, kNodeAddChild), {parent, g_scratch + kScratchRef});
    uint32_t first = R32(parent + kNodeChildFirst);
    for (uint32_t i = ChildCount(parent) - 1; i > index; i--) {
        uint32_t slot = first + i * 4;
        W32(slot, R32(slot - 4));
        W32(slot - 4, node);
    }
}

uint32_t IndexOf(uint32_t parent, uint32_t child) {
    for (uint32_t i = 0; i < ChildCount(parent); i++) {
        if (ChildAt(parent, i) == child) return i;
    }
    return 0;
}

void ApplyRemoveObject(PPCContext& ctx, uint32_t object) {
    uint32_t objects = ObjectsNode();
    uint32_t index = IndexOf(objects, object);
    std::string label = Label("Delete", object);
    CallGuest(ctx, Method(object, kNodeAddRef), {object});
    RemoveChild(ctx, objects, object);
    NoteMapChanged();
    auto undo = [object, index](PPCContext& ctx) {
        if (uint32_t parent = ObjectsNode()) Reinsert(ctx, parent, object, index);
        g_stale = true;
    };
    auto forget = [object](PPCContext& ctx) {
        uint32_t kept = object;
        Release(ctx, kept);
    };
    PushUndo(ctx, label, undo, forget);
}

void AddObjectWithUndo(PPCContext& ctx, const std::string& type, const std::array<float, 3>& pos) {
    uint32_t node = ApplyAddObject(ctx, type, pos);
    if (!node) return;
    NoteMapChanged();
    auto undo = [node](PPCContext& ctx) {
        if (IsAiObject(node)) RemoveChild(ctx, ObjectsNode(), node);
        g_stale = true;
    };
    PushUndo(ctx, Label("Add", node), undo);
}

// Pos moved, Angles turned, points moved with Pos and turned about it by the z turn
void ApplyPlacement(PPCContext& ctx, uint32_t object, const std::array<float, 3>& move,
                    const std::array<float, 3>& turn) {
    std::vector<XmlAttribute> attributes = Attributes(object);
    std::array<float, 3> pos{};
    if (!ParseVec3(Find(attributes, "Pos"), pos.data())) return;
    std::array<float, 3> angles{};
    bool has_angles = ParseVec3(Find(attributes, "Angles"), angles.data());
    std::array<float, 3> moved = {pos[0] + move[0], pos[1] + move[1], pos[2] + move[2]};
    SetAttr(ctx, object, "Pos", FormatVec3(moved));
    if (has_angles || turn != std::array<float, 3>{}) {
        for (int i = 0; i < 3; i++) angles[size_t(i)] += turn[size_t(i)];
        SetAttr(ctx, object, "Angles", FormatVec3(angles));
    }
    float radians = turn[2] * 3.14159265f / 180.0f;
    float c = std::cos(radians);
    float s = std::sin(radians);
    uint32_t points = ChildTagged(object, kPoints);
    for (uint32_t i = 0; points && i < ChildCount(points); i++) {
        uint32_t point = ChildAt(points, i);
        std::array<float, 3> p{};
        if (!ParseVec3(Find(Attributes(point), "Pos"), p.data())) continue;
        float x = p[0] - pos[0];
        float y = p[1] - pos[1];
        std::array<float, 3> placed = {moved[0] + x * c - y * s, moved[1] + x * s + y * c, p[2] + move[2]};
        SetAttr(ctx, point, "Pos", FormatVec3(placed));
    }
}

}  // namespace

namespace fcip::editor {

void KeepMission(PPCContext& ctx, uint32_t node) {
    Keep(ctx, g_mission, node);
    g_stale = true;
    g_first_refresh = true;
}

void KeepLevelData(PPCContext& ctx, uint32_t node) { Keep(ctx, g_level_data, node); }

uint32_t KeptLevelData() { return g_level_data; }

void ForgetMission(PPCContext& ctx) {
    ClearUndo(ctx);
    {
        std::lock_guard lock(g_lock);
        g_edits.clear();
    }
    Release(ctx, g_level_data);
    if (!g_mission) return;
    Release(ctx, g_mission);
    Publish(AiObjects{});
    std::lock_guard lock(g_lock);
    g_loaded = g_snapshot;
}

uint32_t MissionToSave(PPCContext& ctx, uint32_t mission_object) {
    if (!g_mission || !EnsureScratch()) return 0;
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(g_mission, kNodeClone), {g_scratch + kScratchRef, g_mission});
    uint32_t copy = R32(g_scratch + kScratchRef);
    if (!copy) return 0;
    std::vector<uint32_t> ai = TakeAiObjects(ctx, copy);
    W32(g_scratch + kScratchRef, copy);
    CallGuest(ctx, SYM_Mission_WriteEntities, {mission_object, g_scratch + kScratchRef});
    uint32_t objects = LastChildTagged(copy, kObjects);
    for (uint32_t node : ai) {
        W32(g_scratch + kScratchRef, node);
        if (objects) CallGuest(ctx, Method(objects, kNodeAddChild), {objects, g_scratch + kScratchRef});
        else Release(ctx, node);
    }
    AddSavedPrototypes(ctx, copy);
    return copy;
}

void UpdateMission(PPCContext& ctx) {
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    if (!g_mission || !EnsureScratch()) return;
    for (Edit& e : edits) e(ctx);
    if (!edits.empty()) g_stale = true;
    if (!g_stale) return;
    g_stale = false;
    Refresh();
}

std::shared_ptr<const AiObjects> ReadAiObjects() {
    std::lock_guard lock(g_lock);
    return g_snapshot;
}

std::shared_ptr<const AiObjects> ReadLoadedAiObjects() {
    std::lock_guard lock(g_lock);
    return g_loaded;
}

std::string LayerOf(uint32_t entity_id) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    auto found = ai->layers.find(entity_id);
    return found == ai->layers.end() ? std::string() : found->second;
}

std::vector<std::string> AiObjectNames(const std::string& type) {
    std::vector<std::string> names;
    for (const AiObject& o : ReadAiObjects()->objects) {
        if (o.type == type) names.push_back(o.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void SetAiAttribute(uint32_t object, std::vector<int> path, std::string key, std::string value) {
    QueueOn(object, "Set " + key + " of", [=](PPCContext& ctx) {
        if (uint32_t node = FindNode(object, path)) SetAttr(ctx, node, key, value);
    });
}

void AddAiChild(uint32_t object, std::vector<int> path, std::string tag) {
    QueueOn(object, "Add " + tag + " to", [=](PPCContext& ctx) {
        if (uint32_t node = FindNode(object, path)) NewChild(ctx, node, tag);
    });
}

void RemoveAiChild(uint32_t object, std::vector<int> path) {
    QueueOn(object, "Remove from", [=](PPCContext& ctx) { ApplyRemove(ctx, object, path); });
}

void MoveAiChild(uint32_t object, std::vector<int> path, int delta) {
    QueueOn(object, "Reorder", [=](PPCContext&) { ApplyMove(object, path, delta); });
}

void AddAiObject(std::string type, std::array<float, 3> pos) {
    Queue([=](PPCContext& ctx) { AddObjectWithUndo(ctx, type, pos); });
}

void RemoveAiObject(uint32_t object) {
    Queue([=](PPCContext& ctx) {
        if (IsAiObject(object)) ApplyRemoveObject(ctx, object);
    });
}

void SetAiPoints(uint32_t object, std::vector<std::array<float, 3>> points) {
    QueueOn(object, "Reshape", [=](PPCContext& ctx) {
        uint32_t list = ChildTagged(object, kPoints);
        if (list) RemoveChild(ctx, object, list);
        list = NewChild(ctx, object, kPoints);
        for (const auto& p : points) {
            if (uint32_t point = list ? NewChild(ctx, list, "Point") : 0) SetAttr(ctx, point, "Pos", FormatVec3(p));
        }
        // the campaign's areas start their points at Pos
        if (!points.empty()) SetAttr(ctx, object, "Pos", FormatVec3(points.front()));
    });
}

void SetAiEntities(uint32_t object, std::vector<std::string> names) {
    QueueOn(object, "Set the entities of", [=](PPCContext& ctx) {
        uint32_t list = ChildTagged(object, kEntities);
        if (list) RemoveChild(ctx, object, list);
        if (names.empty()) return;
        list = NewChild(ctx, object, kEntities);
        for (const std::string& name : names) {
            if (uint32_t entity = list ? NewChild(ctx, list, kEntity) : 0) SetAttr(ctx, entity, "Name", name);
        }
    });
}

void PlaceAiObject(uint32_t object, std::array<float, 3> move, std::array<float, 3> turn) {
    QueueOn(object, "Move", [=](PPCContext& ctx) { ApplyPlacement(ctx, object, move, turn); });
}

}  // namespace fcip::editor
