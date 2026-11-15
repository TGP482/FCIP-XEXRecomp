// ige.xex, the solo level editor: the level's cutscenes and scripted sequences. As a level loads, its movie system
// reads moviedata.xml from the level's folder (823AEFE0, as 8229B940 starts the level); the editor reads the same file
// once more for the view. Each sequence moves its nodes, the entities NodeData names, along position keys (track
// ParamId 1, a vector track as 82529108 makes it) and sends them events (ParamId 4). The view draws each moved
// entity's keys joined in order, from where the sequence first places it, with the events it gets near the camera.
// A trigger's PlaySequence and _Mission_PlaySequence point at where the sequence starts.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/markers.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr int kTrackPosition = 1;
constexpr int kTrackEvents = 4;

// guest scratch: the loaded node's ref, the loader's error text (std::string), the path
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchError = 0x04;
constexpr uint32_t kScratchPath = 0x20;
constexpr uint32_t kScratchPathBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchPath + kScratchPathBytes;
constexpr uint32_t kStrText = 0x04;
constexpr uint32_t kStrLength = 0x14;
constexpr uint32_t kStrCapacity = 0x18;
constexpr uint32_t kStrEmptyCapacity = 0x0F;

constexpr ImU32 kPathColour = IM_COL32(220, 120, 250, 255);
constexpr ImU32 kKeyColour = IM_COL32(220, 120, 250, 150);

struct Key {
    float time = 0.0f;
    float pos[3] = {};
};
struct MovieNode {
    std::string name;  // the entity it moves
    std::vector<Key> path;
    std::vector<std::string> events;  // "3s Enable"
};
struct Sequence {
    std::string name;
    float start_time = 0.0f;
    float end_time = 0.0f;
    std::vector<MovieNode> nodes;
    bool has_start = false;
    float start[3] = {};
};
struct NodeInfo {
    std::string name;
    float pos[3] = {};
};
using Movie = std::vector<Sequence>;

std::mutex g_lock;
std::shared_ptr<const Movie> g_movie = std::make_shared<Movie>();
uint32_t g_scratch = 0;  // editor thread

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

std::string Attr(uint32_t node, const char* key) {
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        if (GuestString(a + kAttrKey) == key) return GuestString(a + kAttrValue);
    }
    return {};
}

float FloatAttr(uint32_t node, const char* key) { return std::strtof(Attr(node, key).c_str(), nullptr); }

bool Vec3Attr(uint32_t node, const char* key, float out[3]) {
    return std::sscanf(Attr(node, key).c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

uint32_t ChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        if (Tag(ChildAt(node, i)) == tag) return ChildAt(node, i);
    }
    return 0;
}

void ReadTrack(uint32_t track, MovieNode& out) {
    int param = std::atoi(Attr(track, "ParamId").c_str());
    for (uint32_t i = 0; i < ChildCount(track); i++) {
        uint32_t key = ChildAt(track, i);
        float time = FloatAttr(key, "time");
        Key k{time, {}};
        if (param == kTrackPosition && Vec3Attr(key, "value", k.pos)) out.path.push_back(k);
        if (param != kTrackEvents) continue;
        char text[32];
        std::snprintf(text, sizeof(text), "%gs ", time);
        out.events.push_back(text + Attr(key, "event"));
    }
}

Sequence ReadSequence(uint32_t node, const std::unordered_map<std::string, NodeInfo>& infos) {
    Sequence s;
    s.name = Attr(node, "Name");
    s.start_time = FloatAttr(node, "StartTime");
    s.end_time = FloatAttr(node, "EndTime");
    uint32_t nodes = ChildTagged(node, "Nodes");
    for (uint32_t i = 0; nodes && i < ChildCount(nodes); i++) {
        uint32_t child = ChildAt(nodes, i);
        auto info = infos.find(Attr(child, "Id"));
        MovieNode n;
        if (info != infos.end()) n.name = info->second.name;
        for (uint32_t t = 0; t < ChildCount(child); t++) ReadTrack(ChildAt(child, t), n);
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
Movie ReadMovie(uint32_t root) {
    Movie movie;
    uint32_t mission = ChildTagged(root, "Mission");
    uint32_t node_data = mission ? ChildTagged(mission, "NodeData") : 0;
    uint32_t sequence_data = mission ? ChildTagged(mission, "SequenceData") : 0;
    std::unordered_map<std::string, NodeInfo> infos;
    for (uint32_t i = 0; node_data && i < ChildCount(node_data); i++) {
        uint32_t node = ChildAt(node_data, i);
        NodeInfo info{Attr(node, "Name"), {}};
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

void KeepMovie(PPCContext& ctx, const std::string& path) {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    if (!g_scratch) return;
    W32(g_scratch + kScratchRef, 0);
    W8(g_scratch + kScratchError + kStrText, 0);
    W32(g_scratch + kScratchError + kStrLength, 0);
    W32(g_scratch + kScratchError + kStrCapacity, kStrEmptyCapacity);
    uint32_t guest_path = WriteText(g_scratch + kScratchPath, path, kScratchPathBytes);
    CallGuest(ctx, SYM_Xml_LoadFile, {g_scratch + kScratchRef, g_scratch + kScratchError, guest_path, 0});
    CallGuest(ctx, SYM_String_Destroy, {g_scratch + kScratchError});
    uint32_t root = R32(g_scratch + kScratchRef);
    if (!root) return;
    Publish(ReadMovie(root));
    CallGuest(ctx, Method(root, kNodeRelease), {root});
}

std::shared_ptr<const Movie> CurrentMovie() {
    std::lock_guard lock(g_lock);
    return g_movie;
}

// a node without keys is labelled where its entity stands
void DrawNode(const View& v, const Sequence& s, const MovieNode& n, const float* entity) {
    for (size_t i = 0; i < n.path.size(); i++) {
        Square(v, n.path[i].pos, kKeyColour);
        if (i > 0) Arrow(v, n.path[i - 1].pos, n.path[i].pos, kPathColour);
    }
    const float* at = n.path.empty() ? entity : n.path.front().pos;
    if (!at || !NearCamera(v, at) || (n.path.empty() && n.events.empty())) return;
    char head[160];
    std::snprintf(head, sizeof(head), "%s: %s (%g to %gs)", s.name.c_str(), n.name.c_str(), s.start_time, s.end_time);
    std::string text = head;
    for (const std::string& e : n.events) text += "\n" + e;
    Label(v, at, text.c_str(), kPathColour);
}

void ForgetMovie() { Publish({}); }

}  // namespace

namespace fcip::editor {

std::vector<std::string> SequenceNames() {
    std::vector<std::string> names;
    for (const Sequence& s : *CurrentMovie()) names.push_back(s.name);
    std::sort(names.begin(), names.end());
    return names;
}

bool SequenceStart(const std::string& name, float out[3]) {
    for (const Sequence& s : *CurrentMovie()) {
        if (s.name != name || !s.has_start) continue;
        std::copy(s.start, s.start + 3, out);
        return true;
    }
    return false;
}

void DrawSequences(const View& v) {
    std::shared_ptr<const std::vector<ViewEntity>> entities = ViewEntities();
    std::unordered_map<std::string, const float*> places;
    for (const ViewEntity& e : *entities) places[e.name] = e.pos;
    for (const Sequence& s : *CurrentMovie()) {
        for (const MovieNode& n : s.nodes) {
            auto place = places.find(n.name);
            DrawNode(v, s, n, place == places.end() ? nullptr : place->second);
        }
    }
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_823AEFE0);

// the movie system reads a level's sequences (r3 the movie system, r4 the file's path)
PPC_FUNC(sub_823AEFE0) {
    if (!g_solo) return __imp__sub_823AEFE0(ctx, base);
    std::string path = ReadCString(uint32_t(ctx.r[4]));
    ForgetMovie();
    __imp__sub_823AEFE0(ctx, base);
    KeepMovie(ctx, path);
}
