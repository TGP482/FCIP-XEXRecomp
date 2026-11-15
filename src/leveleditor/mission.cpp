// ige.xex, the solo level editor: a campaign level's mission XML and LevelData.xml stay referenced after it loads. The
// mission's Objects hold the AI objects (tag points, anchors with their actions, shapes, areas), which the engine reads
// into the AI system only as the level starts (8229CD10). The panel edits them here, and the custom level save writes
// the mission back with the editor's entities in place of the campaign's.
#include "leveleditor/mission.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kernel/memory.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/guest.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kObjects = "Objects";
constexpr const char* kObject = "Object";
constexpr const char* kEntity = "Entity";
constexpr const char* kPoints = "Points";
constexpr const char* kEntities = "Entities";

// what a new object gets after Type, Name and Pos, as the campaign's missions write them
struct Default {
    const char* key;
    const char* value;
};
constexpr Default kTagPointDefaults[] = {{"Angles", "0,0,0"}, {"Action", ""}};
constexpr Default kAnchorDefaults[] = {
    {"Angles", "0,0,0"}, {"Action", "AIANCHOR_GENERIC_JOB"}, {"Enabled", "1"}, {"AutoDisable", "1"},
    {"GroupID", "-1"},   {"Range", "-1"},                    {"MaxUsers", "1"}, {"NextAnchor", ""},
    {"AnchorId", "9"},   {"Area", "0"},
};

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
std::vector<Edit> g_edits;
// editor thread
uint32_t g_mission = 0;
uint32_t g_level_data = 0;
bool g_stale = false;
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

template <size_t N>
void SetDefaults(PPCContext& ctx, uint32_t node, const Default (&defaults)[N]) {
    for (const Default& d : defaults) SetAttr(ctx, node, d.key, d.value);
}

void ApplyAddObject(PPCContext& ctx, const std::string& type, const std::array<float, 3>& pos) {
    uint32_t objects = ObjectsNode();
    uint32_t node = objects ? NewChild(ctx, objects, kObject) : 0;
    if (!node) return;
    SetAttr(ctx, node, "Type", type);
    SetAttr(ctx, node, "Name", UniqueName(type));
    SetAttr(ctx, node, "Pos", FormatVec3(pos));
    if (type == "AIAnchor") SetDefaults(ctx, node, kAnchorDefaults);
    else SetDefaults(ctx, node, kTagPointDefaults);
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

// the edit runs only while its object is still one of the mission's
void QueueOn(uint32_t object, std::function<void(PPCContext&)> edit) {
    Queue([object, edit = std::move(edit)](PPCContext& ctx) {
        if (IsAiObject(object)) edit(ctx);
    });
}

}  // namespace

namespace fcip::editor {

void KeepMission(PPCContext& ctx, uint32_t node) {
    Keep(ctx, g_mission, node);
    g_stale = true;
}

void KeepLevelData(PPCContext& ctx, uint32_t node) { Keep(ctx, g_level_data, node); }

uint32_t KeptLevelData() { return g_level_data; }

void ForgetMission(PPCContext& ctx) {
    {
        std::lock_guard lock(g_lock);
        g_edits.clear();
    }
    Release(ctx, g_level_data);
    if (!g_mission) return;
    Release(ctx, g_mission);
    Publish(AiObjects{});
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
    QueueOn(object, [=](PPCContext& ctx) {
        if (uint32_t node = FindNode(object, path)) SetAttr(ctx, node, key, value);
    });
}

void AddAiChild(uint32_t object, std::vector<int> path, std::string tag) {
    QueueOn(object, [=](PPCContext& ctx) {
        if (uint32_t node = FindNode(object, path)) NewChild(ctx, node, tag);
    });
}

void RemoveAiChild(uint32_t object, std::vector<int> path) {
    QueueOn(object, [=](PPCContext& ctx) { ApplyRemove(ctx, object, path); });
}

void MoveAiChild(uint32_t object, std::vector<int> path, int delta) {
    QueueOn(object, [=](PPCContext&) { ApplyMove(object, path, delta); });
}

void AddAiObject(std::string type, std::array<float, 3> pos) {
    Queue([=](PPCContext& ctx) { ApplyAddObject(ctx, type, pos); });
}

void RemoveAiObject(uint32_t object) {
    QueueOn(object, [=](PPCContext& ctx) { RemoveChild(ctx, ObjectsNode(), object); });
}

}  // namespace fcip::editor
