// ige.xex, the solo level editor: the mission's entity layers. The mission keeps an entity's layer only as its Entity
// Layer attribute and the engine keeps none, so the editor starts from the loaded mission's and the save writes them
// back (event_links.cpp). A hidden layer's models are hidden as the script's Hide does (82130518), which sets the bit
// the editor saves as HiddenInGame, so the save clears it for them.
#include "leveleditor/layers.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

struct Assignments {
    std::map<uint32_t, std::string> of;  // by EntityId
    std::set<std::string> created;       // made in the editor, still empty
};

std::mutex g_lock;
Assignments g_state;
std::set<std::string> g_hidden;
bool g_has_unlayered = false;
std::vector<std::pair<std::string, Assignments>> g_steps;  // each edit's label and what it changed, to push as undo
// editor thread
std::shared_ptr<const AiObjects> g_seeded;
std::set<uint32_t> g_hid;  // entities whose model a hidden layer hid

template <class F>
void Edit(std::string label, F&& change) {
    std::lock_guard lock(g_lock);
    g_steps.emplace_back(std::move(label), g_state);
    change(g_state);
}

// a level's layers start as its mission's
void Seed() {
    std::shared_ptr<const AiObjects> loaded = ReadLoadedAiObjects();
    if (loaded == g_seeded) return;
    g_seeded = loaded;
    g_hid.clear();
    std::lock_guard lock(g_lock);
    g_state = {{loaded->layers.begin(), loaded->layers.end()}, {}};
    g_hidden.clear();
    g_steps.clear();
}

void PushSteps(PPCContext& ctx) {
    std::vector<std::pair<std::string, Assignments>> steps;
    {
        std::lock_guard lock(g_lock);
        steps.swap(g_steps);
    }
    for (auto& [label, before] : steps) {
        PushUndo(ctx, label, [before](PPCContext&) {
            std::lock_guard lock(g_lock);
            g_state = before;
        });
    }
}

// an entity the mission hides stays as it is
void SetModelHidden(PPCContext& ctx, uint32_t entity, bool hide) {
    uint32_t id = R32(entity + kEntityId);
    bool hidden = R8(entity + kEntityFlags) & kEntityHidden;
    if (hide && !hidden) {
        CallGuest(ctx, SYM_Entity_Hide, {entity, 1, 0});
        g_hid.insert(id);
    }
    if (!hide && g_hid.erase(id) && hidden) CallGuest(ctx, SYM_Entity_Hide, {entity, 0, 0});
}

}  // namespace

namespace fcip::editor {

std::string LayerOf(uint32_t entity_id) {
    std::lock_guard lock(g_lock);
    auto found = g_state.of.find(entity_id);
    return found == g_state.of.end() ? std::string() : found->second;
}

std::vector<std::string> ViewLayers() {
    std::lock_guard lock(g_lock);
    std::set<std::string> unique = g_state.created;
    for (const auto& [id, layer] : g_state.of) unique.insert(layer);
    std::vector<std::string> layers(unique.begin(), unique.end());
    if (g_has_unlayered) layers.push_back(kNoLayer);
    return layers;
}

bool LayerShown(const std::string& layer) {
    std::lock_guard lock(g_lock);
    return !g_hidden.count(layer);
}

void ShowLayer(const std::string& layer, bool shown) {
    std::lock_guard lock(g_lock);
    if (shown) g_hidden.erase(layer);
    else g_hidden.insert(layer);
}

void ShowAllLayers(bool shown) {
    std::vector<std::string> layers = ViewLayers();
    std::lock_guard lock(g_lock);
    if (shown) g_hidden.clear();
    else g_hidden.insert(layers.begin(), layers.end());
}

void CreateLayer(const std::string& name) {
    Edit("Create layer " + name, [&](Assignments& a) { a.created.insert(name); });
}

void RenameLayer(const std::string& from, const std::string& to) {
    Edit("Rename layer " + from, [&](Assignments& a) {
        for (auto& [id, layer] : a.of) {
            if (layer == from) layer = to;
        }
        if (a.created.erase(from)) a.created.insert(to);
    });
    std::lock_guard lock(g_lock);
    if (g_hidden.erase(from)) g_hidden.insert(to);
}

void DeleteLayer(const std::string& name) {
    Edit("Delete layer " + name, [&](Assignments& a) {
        std::erase_if(a.of, [&](const auto& entry) { return entry.second == name; });
        a.created.erase(name);
    });
    std::lock_guard lock(g_lock);
    g_hidden.erase(name);
}

void MoveToLayer(uint32_t entity_id, const std::string& layer) {
    bool none = layer.empty() || layer == kNoLayer;
    Edit("Move to layer " + (none ? std::string(kNoLayer) : layer), [&](Assignments& a) {
        if (none) a.of.erase(entity_id);
        else a.of[entity_id] = layer;
    });
}

void UpdateLayers(PPCContext& ctx) {
    Seed();
    PushSteps(ctx);
    std::map<uint32_t, std::string> of;
    std::set<std::string> hidden;
    {
        std::lock_guard lock(g_lock);
        of = g_state.of;
        hidden = g_hidden;
    }
    bool unlayered = false;
    ForEachEntity([&](uint32_t entity) {
        auto found = of.find(R32(entity + kEntityId));
        unlayered |= found == of.end();
        SetModelHidden(ctx, entity, hidden.count(found == of.end() ? kNoLayer : found->second) > 0);
    });
    std::lock_guard lock(g_lock);
    g_has_unlayered = unlayered;
}

void ShowLayerModels(PPCContext& ctx) {
    ForEachEntity([&](uint32_t entity) { SetModelHidden(ctx, entity, false); });
    g_hid.clear();
}

bool ModelHiddenByLayer(uint32_t entity_id) { return g_hid.count(entity_id) > 0; }

}  // namespace fcip::editor
