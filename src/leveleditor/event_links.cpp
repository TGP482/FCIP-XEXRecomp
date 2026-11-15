// ige.xex, the solo level editor: event links. The engine reads a mission entity's <EventTargets> into its script's
// Events table as it loads (8229FC48). The editor's save writes none and numbers EntityId afresh, so the links are kept
// here by entity id, caught as levels load, and written into the save with the targets' new numbers.
#include "leveleditor/event_links.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "kernel/memory.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kEventTargets = "EventTargets";
constexpr const char* kEventTarget = "EventTarget";

// guest scratch: two node refs, then the texts of a call
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchChildRef = 0x04;
constexpr uint32_t kScratchKey = 0x10;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = kScratchKey + kScratchKeyBytes;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchValue + kScratchValueBytes;

// a saved entity: its EntityId in the file and its node
struct Saved {
    uint32_t number = 0;
    uint32_t node = 0;
};

std::mutex g_lock;
std::map<uint32_t, std::vector<Link>> g_links;  // by the source's entity id
std::map<uint32_t, Saved> g_saved;              // the mission being saved, by entity id
uint32_t g_scratch = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

std::string Attr(PPCContext& ctx, uint32_t node, const char* key) {
    uint32_t value = uint32_t(CallGuest(ctx, Method(node, kNodeGetAttr), {node, KeyText(key)}));
    return value ? ReadCString(value) : std::string();
}

void SetAttr(PPCContext& ctx, uint32_t node, const char* key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

void Release(PPCContext& ctx, uint32_t node) {
    if (node) CallGuest(ctx, Method(node, kNodeRelease), {node});
}

// a new child of parent, referenced once
uint32_t NewChild(PPCContext& ctx, uint32_t parent, const char* tag) {
    W32(g_scratch + kScratchChildRef, 0);
    CallGuest(ctx, Method(parent, kNodeNewChild), {g_scratch + kScratchChildRef, parent, KeyText(tag)});
    return R32(g_scratch + kScratchChildRef);
}

// the <EventTargets> node's links, as the engine reads them
std::vector<Link> ReadLinks(PPCContext& ctx, uint32_t node) {
    std::vector<Link> links;
    uint32_t first = R32(node + kNodeChildFirst);
    for (uint32_t c = first; c && c < R32(node + kNodeChildLast); c += 4) {
        uint32_t child = R32(c);
        std::string target = Attr(ctx, child, "Target");
        if (target.empty()) continue;
        Link link;
        link.source = Attr(ctx, child, "SourceEvent");
        link.target = uint32_t(std::strtoul(target.c_str(), nullptr, 10));
        link.event = Attr(ctx, child, "Event");
        link.param = Attr(ctx, child, "ExtraParam");
        links.push_back(link);
    }
    return links;
}

// target names the entity's number in the file
void WriteLink(PPCContext& ctx, uint32_t events, const Link& link, uint32_t target) {
    uint32_t node = NewChild(ctx, events, kEventTarget);
    if (!node) return;
    SetAttr(ctx, node, "Target", std::to_string(target));
    SetAttr(ctx, node, "Event", link.event);
    SetAttr(ctx, node, "SourceEvent", link.source);
    if (!link.param.empty()) SetAttr(ctx, node, "ExtraParam", link.param);
    Release(ctx, node);
}

// each saved entity's links whose target was saved too
void WriteSavedLinks(PPCContext& ctx) {
    std::map<uint32_t, std::vector<Link>> links;
    {
        std::lock_guard lock(g_lock);
        links = g_links;
    }
    for (const auto& [id, saved] : g_saved) {
        auto found = links.find(id);
        if (found == links.end() || found->second.empty()) continue;
        uint32_t events = NewChild(ctx, saved.node, kEventTargets);
        if (!events) continue;
        for (const Link& link : found->second) {
            // target 0 is the mission's own call
            auto target = g_saved.find(link.target);
            if (link.target == 0) WriteLink(ctx, events, link, 0);
            else if (target != g_saved.end()) WriteLink(ctx, events, link, target->second.number);
        }
        Release(ctx, events);
    }
}

}  // namespace

namespace fcip::editor {

void ForgetLinks() {
    std::lock_guard lock(g_lock);
    g_links.clear();
}

std::vector<Link> LinksOf(uint32_t entity_id) {
    std::lock_guard lock(g_lock);
    auto found = g_links.find(entity_id);
    return found == g_links.end() ? std::vector<Link>{} : found->second;
}

std::vector<std::string> LevelMissionCalls() {
    std::set<std::string> calls;
    std::lock_guard lock(g_lock);
    for (const auto& [id, links] : g_links) {
        for (const Link& l : links) {
            if (l.target == 0) calls.insert(l.event);
        }
    }
    return {calls.begin(), calls.end()};
}

void SetLinks(PPCContext& ctx, uint32_t entity, const std::vector<Link>& links) {
    if (!EnsureScratch()) return;
    uint32_t node = uint32_t(CallGuest(ctx, SYM_Memory_Alloc, {0, 0, 0, kNodeBytes, 0}));
    if (!node) return;
    CallGuest(ctx, SYM_XmlNode_Construct, {node, KeyText(kEventTargets)});
    CallGuest(ctx, Method(node, kNodeAddRef), {node});
    for (const Link& link : links) WriteLink(ctx, node, link, link.target);
    // the engine's own reader rebuilds the Events table, and the hook below keeps the links
    W32(g_scratch + kScratchRef, node);
    CallGuest(ctx, SYM_Entity_LoadEvents, {0, entity, g_scratch + kScratchRef});
    Release(ctx, node);
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_8229FC48);
PPC_EXTERN_FUNC(__imp__sub_821043A0);
PPC_EXTERN_FUNC(__imp__sub_82104C38);

// an entity's <EventTargets> into its script's Events table: r4 the entity, r5 the node's XmlNodeRef
PPC_FUNC(sub_8229FC48) {
    if (!fcip::editor::g_solo) return __imp__sub_8229FC48(ctx, base);
    uint32_t entity = uint32_t(ctx.r[4]);
    uint32_t node = R32(uint32_t(ctx.r[5]));
    if (entity && node && EnsureScratch()) {
        std::vector<Link> links = ReadLinks(ctx, node);
        std::lock_guard lock(g_lock);
        g_links[R32(entity + kEntityId)] = std::move(links);
    }
    __imp__sub_8229FC48(ctx, base);
}

// one entity into a mission's Objects (r4 the entity, r5 its XmlNodeRef); writing it counts EntityId up
PPC_FUNC(sub_821043A0) {
    if (!fcip::editor::g_solo) return __imp__sub_821043A0(ctx, base);
    uint32_t entity = uint32_t(ctx.r[4]);
    uint32_t objects = R32(uint32_t(ctx.r[5]));
    uint32_t before = R32(SYM_Save_EntityNumber);
    __imp__sub_821043A0(ctx, base);
    uint32_t number = R32(SYM_Save_EntityNumber);
    uint32_t first = objects ? R32(objects + kNodeChildFirst) : 0;
    uint32_t last = objects ? R32(objects + kNodeChildLast) : 0;
    if (number == before || !first || last <= first) return;
    uint32_t node = R32(last - 4);
    g_saved[R32(entity + kEntityId)] = {number, node};
    SaveArchetype(ctx, entity, node);
    // the editor's writer drops the mission's Layer
    std::string layer = LayerOf(R32(entity + kEntityId));
    if (!layer.empty() && EnsureScratch()) SetAttr(ctx, node, "Layer", layer);
}

// a mission's Objects, entity by entity; then their links, now that every target has its number
PPC_FUNC(sub_82104C38) {
    if (!fcip::editor::g_solo) return __imp__sub_82104C38(ctx, base);
    g_saved.clear();
    __imp__sub_82104C38(ctx, base);
    if (EnsureScratch()) WriteSavedLinks(ctx);
    g_saved.clear();
}
