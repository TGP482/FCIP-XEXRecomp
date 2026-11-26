// ige.xex, the solo level editor: the level's sequences. As a level loads, its movie system reads moviedata.xml from
// the level's folder (823AEFE0, as 8229B940 starts the level); the editor reads the same file and keeps it. Each
// sequence moves its nodes, the entities NodeData names, along position keys (track ParamId 1) and rotation keys (2,
// w x y z) and sends them events (4). Edits change the kept file and hand its mission to the movie system's own
// reader again (823ABFE8, loading, old nodes removed), so play test plays what was edited; the custom level save
// writes it. A preview asks the game's own tracks where each entity is at a time (track GetValue: rotation +0x54,
// vector +0x58, f1 the time) and puts the entities there until it ends.
#include "leveleditor/sequences.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>

#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "leveleditor/picking.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr int kTrackPosition = 1;
constexpr int kTrackRotation = 2;
constexpr int kTrackEvents = 4;
constexpr float kDegrees = 180.0f / 3.14159265f;

// sequence: its nodes as {node, ?} pairs; node: its tracks by ParamId
constexpr uint32_t kSequenceNodesFirst = 0x0C;
constexpr uint32_t kSequenceNodesLast = 0x10;
constexpr uint32_t kNodeEntryBytes = 8;
constexpr uint32_t kNodeTracks = 0x08;
// track methods
constexpr uint32_t kTrackKeyCount = 0x0C;
constexpr uint32_t kTrackQuat = 0x54;
constexpr uint32_t kTrackVec3 = 0x58;
constexpr uint32_t kEntitySetPos = 0x14;  // (entity, Vec3, 0)

// guest scratch: a node ref, the loader's error text (std::string), texts, a value
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchError = 0x04;
constexpr uint32_t kScratchValue = 0x20;  // up to a Quat
constexpr uint32_t kScratchKey = 0x40;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchText = kScratchKey + kScratchKeyBytes;
constexpr uint32_t kScratchTextBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchText + kScratchTextBytes;
constexpr uint32_t kStrText = 0x04;
constexpr uint32_t kStrLength = 0x14;
constexpr uint32_t kStrCapacity = 0x18;
constexpr uint32_t kStrEmptyCapacity = 0x0F;

struct NodeInfo {
    std::string name;
    std::string entity_id;
    float pos[3] = {};
};

using Edit = std::function<void(PPCContext&)>;

struct PreviewRequest {
    bool on = false;
    std::string name;
    float time = 0.0f;
};

struct Placement {
    uint32_t entity = 0;
    float pos[3] = {};
    float angles[3] = {};
};

std::mutex g_lock;
std::shared_ptr<const Movie> g_movie = std::make_shared<Movie>();
std::vector<Edit> g_edits;
PreviewRequest g_request;
// editor thread
uint32_t g_root = 0;
uint32_t g_scratch = 0;
std::string g_previewing;  // the sequence previewed, empty for none
uint32_t g_preview_sequence = 0;
float g_preview_time = -1.0f;
std::vector<Placement> g_moved;
std::atomic<bool> g_kept{false};

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchText, text, kScratchTextBytes); }

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

std::vector<XmlAttribute> Attributes(uint32_t node) {
    std::vector<XmlAttribute> out;
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        out.push_back({GuestString(a + kAttrKey), GuestString(a + kAttrValue)});
    }
    return out;
}

std::string Attr(uint32_t node, const char* key) {
    for (const XmlAttribute& a : Attributes(node)) {
        if (a.key == key) return a.value;
    }
    return {};
}

float FloatAttr(uint32_t node, const char* key) { return std::strtof(Attr(node, key).c_str(), nullptr); }

bool Vec3Attr(uint32_t node, const char* key, float out[3]) {
    return std::sscanf(Attr(node, key).c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

uint32_t ChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = 0; node && i < ChildCount(node); i++) {
        if (Tag(ChildAt(node, i)) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t MissionNode() { return ChildTagged(g_root, "Mission"); }

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

void ReadTrack(uint32_t track, std::vector<int> path, MovieNode& out) {
    int param = std::atoi(Attr(track, "ParamId").c_str());
    for (uint32_t i = 0; i < ChildCount(track); i++) {
        uint32_t key = ChildAt(track, i);
        float time = FloatAttr(key, "time");
        Key k{time, {}, path};
        k.path.push_back(int(i));
        if (param == kTrackPosition && Vec3Attr(key, "value", k.pos)) out.path.push_back(k);
        if (param != kTrackEvents) continue;
        char text[32];
        std::snprintf(text, sizeof(text), "%gs ", time);
        out.events.push_back(text + Attr(key, "event"));
    }
}

Sequence ReadSequence(uint32_t node, const std::unordered_map<std::string, NodeInfo>& infos) {
    Sequence s;
    s.node = node;
    s.name = Attr(node, "Name");
    s.start_time = FloatAttr(node, "StartTime");
    s.end_time = FloatAttr(node, "EndTime");
    ReadElements(node, XmlElement{}, s.elements);
    uint32_t nodes = 0;
    int nodes_index = 0;
    for (uint32_t i = 0; i < ChildCount(node) && !nodes; i++) {
        if (Tag(ChildAt(node, i)) != "Nodes") continue;
        nodes = ChildAt(node, i);
        nodes_index = int(i);
    }
    for (uint32_t i = 0; nodes && i < ChildCount(nodes); i++) {
        uint32_t child = ChildAt(nodes, i);
        MovieNode n;
        n.id = Attr(child, "Id");
        auto info = infos.find(n.id);
        if (info != infos.end()) n.name = info->second.name;
        for (uint32_t t = 0; t < ChildCount(child); t++) {
            ReadTrack(ChildAt(child, t), {nodes_index, int(i), int(t)}, n);
        }
        // where the sequence starts: its first keyed node, else the first node's place
        if (!s.has_start && !n.path.empty()) {
            std::copy(n.path.front().pos, n.path.front().pos + 3, s.start);
            s.has_start = true;
        }
        s.nodes.push_back(std::move(n));
    }
    if (!s.has_start && nodes && ChildCount(nodes)) {
        auto info = infos.find(Attr(ChildAt(nodes, 0), "Id"));
        if (info != infos.end()) std::copy(info->second.pos, info->second.pos + 3, s.start);
        s.has_start = info != infos.end();
    }
    return s;
}

// the first Mission's sequences: a level has one
Movie ReadMovie() {
    Movie movie;
    uint32_t mission = MissionNode();
    uint32_t node_data = ChildTagged(mission, "NodeData");
    uint32_t sequence_data = ChildTagged(mission, "SequenceData");
    std::unordered_map<std::string, NodeInfo> infos;
    for (uint32_t i = 0; node_data && i < ChildCount(node_data); i++) {
        uint32_t node = ChildAt(node_data, i);
        NodeInfo info{Attr(node, "Name"), Attr(node, "EntityId"), {}};
        Vec3Attr(node, "Pos", info.pos);
        infos[Attr(node, "Id")] = info;
    }
    for (uint32_t i = 0; sequence_data && i < ChildCount(sequence_data); i++) {
        movie.push_back(ReadSequence(ChildAt(sequence_data, i), infos));
    }
    return movie;
}

void Publish(Movie movie) {
    auto next = std::make_shared<Movie>(std::move(movie));
    std::lock_guard lock(g_lock);
    g_movie = std::move(next);
}

void Release(PPCContext& ctx, uint32_t node) {
    if (node) CallGuest(ctx, Method(node, kNodeRelease), {node});
}

void KeepMovie(PPCContext& ctx, const std::string& path) {
    if (!EnsureScratch()) return;
    W32(g_scratch + kScratchRef, 0);
    W8(g_scratch + kScratchError + kStrText, 0);
    W32(g_scratch + kScratchError + kStrLength, 0);
    W32(g_scratch + kScratchError + kStrCapacity, kStrEmptyCapacity);
    CallGuest(ctx, SYM_Xml_LoadFile, {g_scratch + kScratchRef, g_scratch + kScratchError, ValueText(path), 0});
    CallGuest(ctx, SYM_String_Destroy, {g_scratch + kScratchError});
    g_root = R32(g_scratch + kScratchRef);
    g_kept = g_root != 0;
    if (g_root) Publish(ReadMovie());
}

// Preview, editor thread

void ReadVec(uint32_t at, float out[3]) {
    for (uint32_t i = 0; i < 3; i++) out[i] = GF(at + i * 4);
}

uint32_t WriteVec(const float v[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t bits;
        std::memcpy(&bits, &v[i], sizeof(bits));
        W32(g_scratch + kScratchValue + i * 4, bits);
    }
    return g_scratch + kScratchValue;
}

// a track's value at the time into the scratch value, false for no track or no keys
bool TrackValue(PPCContext& ctx, uint32_t track, uint32_t method, float time) {
    if (!track || CallGuest(ctx, Method(track, kTrackKeyCount), {track}) == 0) return false;
    double f1 = ctx.f[1];
    ctx.f[1] = time;
    CallGuest(ctx, Method(track, method), {track, g_scratch + kScratchValue});
    ctx.f[1] = f1;
    return true;
}

// the engine's angles in degrees (R = Rz Ry Rx) of a rotation w x y z
void AnglesOfQuat(const float q[4], float out[3]) {
    float w = q[0];
    float x = q[1];
    float y = q[2];
    float z = q[3];
    float r00 = 1.0f - 2.0f * (y * y + z * z);
    float r10 = 2.0f * (x * y + w * z);
    float r20 = 2.0f * (x * z - w * y);
    float r21 = 2.0f * (y * z + w * x);
    float r22 = 1.0f - 2.0f * (x * x + y * y);
    out[0] = std::atan2(r21, r22) * kDegrees;
    out[1] = std::asin(std::clamp(-r20, -1.0f, 1.0f)) * kDegrees;
    out[2] = std::atan2(r10, r00) * kDegrees;
}

uint32_t EntityNamed(const std::string& name) {
    uint32_t found = 0;
    ForEachEntity([&](uint32_t entity) {
        if (!found && GuestString(entity + kEntityName) == name) found = entity;
    });
    return found;
}

void Place(PPCContext& ctx, uint32_t entity, const float pos[3], const float angles[3]) {
    if (pos) CallGuest(ctx, Method(entity, kEntitySetPos), {entity, WriteVec(pos), 0});
    if (angles) CallGuest(ctx, SYM_Entity_SetAngles, {entity, WriteVec(angles), 1, 1, 0});
}

void Restore(PPCContext& ctx) {
    for (const Placement& p : g_moved) Place(ctx, p.entity, p.pos, p.angles);
    g_moved.clear();
    if (g_preview_sequence) CallGuest(ctx, SYM_Sequence_Deactivate, {g_preview_sequence});
    g_preview_sequence = 0;
    g_previewing.clear();
    g_preview_time = -1.0f;
}

void Remember(PPCContext& ctx, uint32_t entity) {
    for (const Placement& p : g_moved) {
        if (p.entity == entity) return;
    }
    Placement p{entity, {}, {}};
    if (uint32_t pos = uint32_t(CallGuest(ctx, Method(entity, kEntityPosition), {entity, 1}))) ReadVec(pos, p.pos);
    ReadVec(entity + kEntityAngles, p.angles);
    g_moved.push_back(p);
}

// the game's nodes are the file's Nodes in order
void PlaceAt(PPCContext& ctx, const Sequence& s, float time) {
    uint32_t first = R32(g_preview_sequence + kSequenceNodesFirst);
    uint32_t count = first ? (R32(g_preview_sequence + kSequenceNodesLast) - first) / kNodeEntryBytes : 0;
    for (uint32_t i = 0; i < count && i < s.nodes.size(); i++) {
        uint32_t node = R32(first + i * kNodeEntryBytes);
        uint32_t entity = node ? EntityNamed(s.nodes[i].name) : 0;
        if (!entity) continue;
        Remember(ctx, entity);
        uint32_t position = R32(node + kNodeTracks + kTrackPosition * 4);
        uint32_t rotation = R32(node + kNodeTracks + kTrackRotation * 4);
        float pos[3];
        float angles[3];
        bool moved = TrackValue(ctx, position, kTrackVec3, time);
        if (moved) ReadVec(g_scratch + kScratchValue, pos);
        float q[4] = {};
        bool turned = TrackValue(ctx, rotation, kTrackQuat, time);
        for (uint32_t k = 0; turned && k < 4; k++) q[k] = GF(g_scratch + kScratchValue + k * 4);
        if (turned) AnglesOfQuat(q, angles);
        Place(ctx, entity, moved ? pos : nullptr, turned ? angles : nullptr);
    }
}

void UpdatePreview(PPCContext& ctx) {
    PreviewRequest want;
    {
        std::lock_guard lock(g_lock);
        want = g_request;
    }
    if (!ShellUp()) want.on = false;
    if (!want.on || want.name != g_previewing) Restore(ctx);
    uint32_t movie = R32(SYM_Movie_pointer);
    if (!want.on || !movie) return;
    if (g_previewing.empty()) {
        g_preview_sequence = uint32_t(CallGuest(ctx, SYM_Movie_FindSequence, {movie, KeyText(want.name)}));
        if (!g_preview_sequence) return;
        CallGuest(ctx, SYM_Sequence_Activate, {g_preview_sequence});
        g_previewing = want.name;
    }
    if (want.time == g_preview_time) return;
    g_preview_time = want.time;
    for (const Sequence& s : *CurrentMovie()) {
        if (s.name == want.name) PlaceAt(ctx, s, want.time);
    }
}

// Edits, editor thread

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

void SetAttr(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

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

uint32_t Clone(PPCContext& ctx, uint32_t node) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(node, kNodeClone), {g_scratch + kScratchRef, node});
    return R32(g_scratch + kScratchRef);
}

// the movie system reads the kept mission again, as the level's load did
void Reload(PPCContext& ctx) {
    Restore(ctx);
    uint32_t movie = R32(SYM_Movie_pointer);
    uint32_t mission = MissionNode();
    if (movie && mission) {
        CallGuest(ctx, SYM_Movie_StopAllSequences, {movie});
        W32(g_scratch + kScratchRef, mission);
        CallGuest(ctx, SYM_Movie_Serialize, {movie, g_scratch + kScratchRef, 1, 1, 0});
    }
    Publish(ReadMovie());
}

uint32_t SequenceData() { return ChildTagged(MissionNode(), "SequenceData"); }

bool IsSequence(uint32_t node) {
    uint32_t data = SequenceData();
    for (uint32_t i = 0; data && i < ChildCount(data); i++) {
        if (ChildAt(data, i) == node) return true;
    }
    return false;
}

// a sequence's content traded back on undo, as mission.cpp does for its objects
void QueueOn(uint32_t sequence, std::string what, std::function<void(PPCContext&)> edit) {
    Queue([=](PPCContext& ctx) {
        if (!IsSequence(sequence)) return;
        uint32_t before = Clone(ctx, sequence);
        edit(ctx);
        Reload(ctx);
        NoteMapChanged();
        if (!before) return;
        auto undo = [sequence, before](PPCContext& ctx) {
            SwapContent(sequence, before);
            Release(ctx, before);
            Reload(ctx);
        };
        auto forget = [before](PPCContext& ctx) { Release(ctx, before); };
        PushUndo(ctx, what + " in " + Attr(sequence, "Name"), undo, forget);
    });
}

// the NodeData node moving the entity, made when the mission has none
std::string NodeIdFor(PPCContext& ctx, uint32_t entity) {
    uint32_t node_data = ChildTagged(MissionNode(), "NodeData");
    if (!node_data) node_data = NewChild(ctx, MissionNode(), "NodeData");
    std::string entity_id = std::to_string(R32(entity + kEntityId));
    for (uint32_t i = 0; i < ChildCount(node_data); i++) {
        if (Attr(ChildAt(node_data, i), "EntityId") == entity_id) return Attr(ChildAt(node_data, i), "Id");
    }
    // the campaign's ids are random 32 bit numbers
    static std::mt19937 random{std::random_device{}()};
    std::string id = std::to_string(int32_t(random()));
    uint32_t info = NewChild(ctx, node_data, "Node");
    float pos[3] = {};
    if (uint32_t at = uint32_t(CallGuest(ctx, Method(entity, kEntityPosition), {entity, 1}))) ReadVec(at, pos);
    char text[96];
    std::snprintf(text, sizeof(text), "%g,%g,%g", pos[0], pos[1], pos[2]);
    for (const XmlAttribute& a : std::vector<XmlAttribute>{{"Id", id}, {"Type", "1"},
                                                          {"Name", GuestString(entity + kEntityName)},
                                                          {"EntityId", entity_id}, {"Pos", text},
                                                          {"Rotate", "1,0,0,0"}, {"Scale", "1,1,1"}}) {
        SetAttr(ctx, info, a.key, a.value);
    }
    return id;
}

void ApplyAddEntity(PPCContext& ctx, uint32_t sequence, uint32_t entity_id) {
    uint32_t entity = EntityOfId(entity_id);
    if (!entity) return;
    uint32_t nodes = ChildTagged(sequence, "Nodes");
    if (!nodes) nodes = NewChild(ctx, sequence, "Nodes");
    uint32_t node = NewChild(ctx, nodes, "Node");
    SetAttr(ctx, node, "Id", NodeIdFor(ctx, entity));
    for (int param : {kTrackPosition, kTrackRotation}) {
        uint32_t track = NewChild(ctx, node, "Track");
        SetAttr(ctx, track, "ParamId", std::to_string(param));
        SetAttr(ctx, track, "Flags", "0");
    }
}

}  // namespace

namespace fcip::editor {

std::shared_ptr<const Movie> CurrentMovie() {
    std::lock_guard lock(g_lock);
    return g_movie;
}

uint32_t KeptMovie() { return g_root; }

bool MovieKept() { return g_kept; }

void UpdateSequences(PPCContext& ctx) {
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    if (!g_root || !EnsureScratch()) return;
    for (Edit& e : edits) e(ctx);
    UpdatePreview(ctx);
}

void SetMovieAttribute(uint32_t sequence, std::vector<int> path, std::string key, std::string value) {
    QueueOn(sequence, "Set " + key, [=](PPCContext& ctx) {
        if (uint32_t node = FindNode(sequence, path)) SetAttr(ctx, node, key, value);
    });
}

void AddMovieChild(uint32_t sequence, std::vector<int> path, std::string tag, std::vector<XmlAttribute> attributes,
                   int index) {
    QueueOn(sequence, "Add " + tag, [=](PPCContext& ctx) {
        uint32_t parent = FindNode(sequence, path);
        uint32_t child = parent ? NewChild(ctx, parent, tag) : 0;
        if (!child) return;
        for (const XmlAttribute& a : attributes) SetAttr(ctx, child, a.key, a.value);
        // moved back from last to index
        uint32_t first = R32(parent + kNodeChildFirst);
        for (uint32_t i = ChildCount(parent) - 1; index >= 0 && i > uint32_t(index); i--) {
            W32(first + i * 4, R32(first + (i - 1) * 4));
            W32(first + (i - 1) * 4, child);
        }
    });
}

void RemoveMovieChild(uint32_t sequence, std::vector<int> path) {
    QueueOn(sequence, "Remove", [=](PPCContext& ctx) {
        uint32_t child = FindNode(sequence, path);
        std::vector<int> parent_path = path;
        parent_path.pop_back();
        uint32_t parent = FindNode(sequence, parent_path);
        if (child && parent && child != sequence) RemoveChild(ctx, parent, child);
    });
}

void AddSequenceEntity(uint32_t sequence, uint32_t entity_id) {
    QueueOn(sequence, "Add an entity", [=](PPCContext& ctx) { ApplyAddEntity(ctx, sequence, entity_id); });
}

void AddSequence(std::string name) {
    Queue([=](PPCContext& ctx) {
        uint32_t data = SequenceData();
        if (!data && MissionNode()) data = NewChild(ctx, MissionNode(), "SequenceData");
        uint32_t sequence = data ? NewChild(ctx, data, "Sequence") : 0;
        if (!sequence) return;
        for (const XmlAttribute& a : std::vector<XmlAttribute>{{"Name", name}, {"Flags", "0"}, {"StartTime", "0"},
                                                              {"EndTime", "10"}}) {
            SetAttr(ctx, sequence, a.key, a.value);
        }
        NewChild(ctx, sequence, "Nodes");
        Reload(ctx);
        NoteMapChanged();
        PushUndo(ctx, "Add sequence " + name, [sequence](PPCContext& ctx) {
            if (IsSequence(sequence)) RemoveChild(ctx, SequenceData(), sequence);
            Reload(ctx);
        });
    });
}

void RemoveSequence(uint32_t sequence) {
    Queue([=](PPCContext& ctx) {
        if (!IsSequence(sequence)) return;
        std::string name = Attr(sequence, "Name");
        CallGuest(ctx, Method(sequence, kNodeAddRef), {sequence});
        RemoveChild(ctx, SequenceData(), sequence);
        Reload(ctx);
        NoteMapChanged();
        auto undo = [sequence](PPCContext& ctx) {
            uint32_t data = SequenceData();
            W32(g_scratch + kScratchRef, sequence);
            if (data) CallGuest(ctx, Method(data, kNodeAddChild), {data, g_scratch + kScratchRef});
            Reload(ctx);
        };
        auto forget = [sequence](PPCContext& ctx) { Release(ctx, sequence); };
        PushUndo(ctx, "Delete sequence " + name, undo, forget);
    });
}

void PreviewSequence(const std::string& name, float time) {
    std::lock_guard lock(g_lock);
    g_request = {true, name, time};
}

void EndPreview() {
    std::lock_guard lock(g_lock);
    g_request = {};
}

void StopPreview(PPCContext& ctx) {
    if (EnsureScratch()) Restore(ctx);
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_823AEFE0);

// the movie system reads a level's sequences (r3 the movie system, r4 the file's path)
PPC_FUNC(sub_823AEFE0) {
    if (!g_solo) return __imp__sub_823AEFE0(ctx, base);
    std::string path = ReadCString(uint32_t(ctx.r[4]));
    g_moved.clear();
    g_previewing.clear();
    g_preview_sequence = 0;
    Release(ctx, std::exchange(g_root, 0u));
    g_kept = false;
    Publish({});
    __imp__sub_823AEFE0(ctx, base);
    KeepMovie(ctx, path);
}
