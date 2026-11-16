// ige.xex, the solo level editor: the Archetypes tab edits the editor's archetype library (every campaign mission's
// EntityLibrary, gathered as the editor starts). An archetype is an <EntityPrototype> with a Name (library.group.name),
// a Class, a Description and its Properties, which an entity loading with it takes before its own LocalProperties
// (8229F090; Properties2 of a prototype is not read). The kept mission's EntityLibrary entry of the same name is
// edited with it, and the save adds the rest an entity names (archetypes.cpp). An entity saves its whole table, so
// an edit reaches the entities already made from the archetype only when Apply to Its Entities sets them again.
#include "leveleditor/archetype_library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/fields.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/node_edit.h"
#include "leveleditor/object_panel.h"
#include "leveleditor/picking.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kEntityLibrary = "EntityLibrary";
constexpr const char* kPrototype = "EntityPrototype";
constexpr const char* kProperties = "Properties";
constexpr const char* kName = "Name";
constexpr const char* kId = "Id";
constexpr const char* kClass = "Class";
constexpr const char* kDescription = "Description";

// guest scratch: a node reference
constexpr uint32_t kScratchRef = 0x0;
constexpr uint32_t kScratchBytes = 0x4;

constexpr size_t kTextChars = 128;

struct Entry {
    std::string name;
    std::string cls;
    std::string description;
    size_t users = 0;  // entities made from it
};
// a table of the picked archetype's Properties: its sub table names from Properties, and its values
struct Table {
    std::vector<std::string> path;
    std::vector<XmlAttribute> attributes;
};
struct Snapshot {
    std::vector<Entry> entries;
    std::string picked;
    std::vector<Table> tables;
    uint32_t generation = 0;
};
using Edit = std::function<void(PPCContext&)>;

std::mutex g_lock;
Snapshot g_snapshot;
std::vector<Edit> g_edits;
std::string g_wanted;  // the archetype the panel picked
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown_library = 0;
std::string g_shown_pick;
bool g_stale = false;
// window thread
Snapshot g_view;
char g_filter[kTextChars] = {};
char g_new_name[kTextChars] = {};

// Editor thread

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

uint32_t LibraryRoot(PPCContext& ctx) {
    uint32_t library = uint32_t(CallGuest(ctx, SYM_Library_Get, {}));
    return library ? R32(library) : 0;
}

uint32_t PrototypeIn(uint32_t list, const std::string& name) {
    for (uint32_t i = 0; list && i < ChildCount(list); i++) {
        uint32_t node = ChildAt(list, i);
        if (Tag(node) == kPrototype && XmlValue(node, kName) == name) return node;
    }
    return 0;
}

// the library's archetype and the kept mission's of the same name, either 0 when missing
std::vector<uint32_t> PrototypesNamed(PPCContext& ctx, const std::string& name) {
    std::vector<uint32_t> out;
    if (uint32_t node = PrototypeIn(LibraryRoot(ctx), name)) out.push_back(node);
    if (uint32_t node = PrototypeIn(XmlChild(KeptMission(), kEntityLibrary), name)) out.push_back(node);
    return out;
}

uint32_t TableAt(uint32_t prototype, const std::vector<std::string>& path) {
    uint32_t node = XmlChild(prototype, kProperties);
    for (size_t i = 0; node && i < path.size(); i++) node = XmlChild(node, path[i]);
    return node;
}

void ReadTables(uint32_t node, std::vector<std::string> path, std::vector<Table>& out) {
    out.push_back({path, XmlAttributes(node)});
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        std::vector<std::string> sub = path;
        sub.push_back(Tag(ChildAt(node, i)));
        ReadTables(ChildAt(node, i), sub, out);
    }
}

Snapshot ReadLibrary(PPCContext& ctx, const std::string& picked) {
    Snapshot s;
    uint32_t root = LibraryRoot(ctx);
    for (uint32_t i = 0; root && i < ChildCount(root); i++) {
        uint32_t node = ChildAt(root, i);
        if (Tag(node) != kPrototype) continue;
        std::string name = XmlValue(node, kName);
        size_t users = EntitiesOfArchetype(name).size();
        s.entries.push_back({name, XmlValue(node, kClass), XmlValue(node, kDescription), users});
    }
    std::sort(s.entries.begin(), s.entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    s.picked = picked;
    if (uint32_t properties = XmlChild(PrototypeIn(root, picked), kProperties)) ReadTables(properties, {}, s.tables);
    return s;
}

void Changed() {
    g_stale = true;
    NoteMapChanged();
}

// a Properties value (path empty) or a sub table's, in the library and the kept mission
void SetValue(PPCContext& ctx, std::string name, std::vector<std::string> path, std::string key, std::string value,
              bool undoable) {
    std::vector<uint32_t> prototypes = PrototypesNamed(ctx, name);
    if (prototypes.empty()) return;
    std::string old = XmlValue(TableAt(prototypes.front(), path), key);
    for (uint32_t p : prototypes) SetXmlValue(ctx, TableAt(p, path), key, value);
    Changed();
    if (!undoable || old == value) return;
    PushUndo(ctx, "Set " + key + " of " + name,
             [=](PPCContext& ctx) { SetValue(ctx, name, path, key, old, false); });
}

void SetDescription(PPCContext& ctx, std::string name, std::string value, bool undoable) {
    std::vector<uint32_t> prototypes = PrototypesNamed(ctx, name);
    if (prototypes.empty()) return;
    std::string old = XmlValue(prototypes.front(), kDescription);
    for (uint32_t p : prototypes) SetXmlValue(ctx, p, kDescription, value);
    Changed();
    if (!undoable || old == value) return;
    PushUndo(ctx, "Describe " + name, [=](PPCContext& ctx) { SetDescription(ctx, name, old, false); });
}

// a node the parent drops, kept alive by the reference the undo holds
uint32_t Detach(PPCContext& ctx, uint32_t parent, uint32_t node) {
    CallGuest(ctx, Method(node, kNodeAddRef), {node});
    RemoveXmlChild(ctx, parent, node);
    return node;
}

// the parent takes the reference the undo held
void Attach(PPCContext& ctx, uint32_t parent, uint32_t node) {
    W32(g_scratch + kScratchRef, node);
    CallGuest(ctx, Method(parent, kNodeAddChild), {parent, g_scratch + kScratchRef});
}

std::string NewGuid() {
    std::random_device device;
    std::mt19937 random(device());
    uint32_t v[4] = {random(), random(), random(), random()};
    char text[48];
    std::snprintf(text, sizeof(text), "{%08X-%04X-%04X-%04X-%04X%08X}", v[0], v[1] >> 16, v[1] & 0xFFFF, v[2] >> 16,
                  v[2] & 0xFFFF, v[3]);
    return text;
}

// a new archetype made from the picked entity: its class and its whole Properties table
void CreateFrom(PPCContext& ctx, uint32_t entity_id, std::string name) {
    uint32_t root = LibraryRoot(ctx);
    uint32_t entity = EntityOfId(entity_id);
    if (!root || !entity || !EnsureScratch() || PrototypeIn(root, name)) return;
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    uint32_t node = AddXmlChild(ctx, root, kPrototype);
    if (!node) return;
    SetXmlValue(ctx, node, kName, name);
    SetXmlValue(ctx, node, kId, NewGuid());
    SetXmlValue(ctx, node, kClass, cls ? ReadCString(cls) : std::string());
    SetXmlValue(ctx, node, kDescription, "");
    if (uint32_t properties = AddXmlChild(ctx, node, kProperties)) WritePropertiesTo(ctx, entity, properties);
    Changed();
    PushUndo(ctx, "Create the archetype " + name, [root, node](PPCContext& ctx) {
        RemoveXmlChild(ctx, root, node);
        Changed();
    });
}

// an archetype no entity uses, out of the library and the kept mission
void Delete(PPCContext& ctx, std::string name) {
    if (!EnsureScratch() || !EntitiesOfArchetype(name).empty()) return;
    uint32_t root = LibraryRoot(ctx);
    uint32_t list = XmlChild(KeptMission(), kEntityLibrary);
    uint32_t in_root = PrototypeIn(root, name);
    uint32_t in_list = PrototypeIn(list, name);
    if (!in_root) return;
    Detach(ctx, root, in_root);
    if (in_list) Detach(ctx, list, in_list);
    Changed();
    auto undo = [=](PPCContext& ctx) {
        Attach(ctx, root, in_root);
        if (in_list) Attach(ctx, list, in_list);
        Changed();
    };
    auto forget = [=](PPCContext& ctx) {
        CallGuest(ctx, Method(in_root, kNodeRelease), {in_root});
        if (in_list) CallGuest(ctx, Method(in_list, kNodeRelease), {in_list});
    };
    PushUndo(ctx, "Delete the archetype " + name, undo, forget);
}

// every entity made from the archetype takes its Properties again, one step of undo each
void ApplyToEntities(PPCContext& ctx, std::string name) {
    for (uint32_t id : EntitiesOfArchetype(name)) {
        if (uint32_t entity = EntityOfId(id)) SetArchetype(ctx, entity, name);
    }
    Changed();
}

// Panel, window thread

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

void Pick(const std::string& name) {
    std::lock_guard lock(g_lock);
    g_wanted = name;
}

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(uint8_t(c)));
    return text;
}

void DrawList() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter", g_filter, sizeof(g_filter));
    std::string filter = Lower(g_filter);
    ImGui::BeginChild("##list", ImVec2(0, ImGui::GetFontSize() * 14), ImGuiChildFlags_Border);
    for (const Entry& e : g_view.entries) {
        std::string label = e.name + " (" + e.cls + ")";
        if (!filter.empty() && Lower(label).find(filter) == std::string::npos) continue;
        if (ImGui::Selectable(label.c_str(), e.name == g_view.picked)) Pick(e.name);
    }
    ImGui::EndChild();
}

const Entry* Picked() {
    for (const Entry& e : g_view.entries) {
        if (e.name == g_view.picked) return &e;
    }
    return nullptr;
}

void DrawTables(const std::string& name) {
    float label_w = ImGui::GetFontSize() * 12;
    for (size_t t = 0; t < g_view.tables.size(); t++) {
        const Table& table = g_view.tables[t];
        std::string title = kProperties;
        for (const std::string& p : table.path) title += " / " + p;
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::PushID(int(t));
        for (const XmlAttribute& a : table.attributes) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.key.c_str());
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-1);
            std::string edited;
            if (!DrawPropertyValue(a.key.c_str(), a.key, a.value, edited)) continue;
            Queue([=, path = table.path, key = a.key](PPCContext& ctx) {
                SetValue(ctx, name, path, key, edited, true);
            });
        }
        ImGui::PopID();
    }
}

void DrawPickedArchetype() {
    const Entry* e = Picked();
    if (!e) return;
    std::string name = e->name;
    ImGui::Text("%s, class %s, %zu entities made from it", name.c_str(), e->cls.c_str(), e->users);
    ImGui::BeginDisabled(e->users == 0);
    if (ImGui::Button("Apply to Its Entities")) Queue([name](PPCContext& ctx) { ApplyToEntities(ctx, name); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(e->users != 0);
    if (ImGui::Button("Delete")) Queue([name](PPCContext& ctx) { Delete(ctx, name); });
    ImGui::EndDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kDescription);
    ImGui::SameLine(ImGui::GetFontSize() * 12);
    ImGui::SetNextItemWidth(-1);
    std::string edited;
    if (DrawField("##description", FieldKind::Text, e->description, edited)) {
        Queue([name, edited](PPCContext& ctx) { SetDescription(ctx, name, edited, true); });
    }
    DrawTables(name);
}

// a new archetype from the entity the Edit Object tool picked
void DrawCreate() {
    uint32_t entity = SelectedEntityId();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##newname", "library.group.name", g_new_name, sizeof(g_new_name));
    ImGui::SameLine();
    std::string name = g_new_name;
    bool taken = false;
    for (const Entry& e : g_view.entries) taken = taken || e.name == name;
    ImGui::BeginDisabled(!entity || name.empty() || taken);
    if (ImGui::Button("Create from Picked Entity")) {
        Queue([entity, name](PPCContext& ctx) { CreateFrom(ctx, entity, name); });
        g_new_name[0] = 0;
    }
    ImGui::EndDisabled();
}

}  // namespace

void fcip::editor::UpdateArchetypeLibrary(PPCContext& ctx) {
    std::vector<Edit> edits;
    std::string wanted;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
        wanted = g_wanted;
    }
    for (Edit& e : edits) e(ctx);
    uint32_t library = LibraryRoot(ctx);
    if (library == g_shown_library && wanted == g_shown_pick && !g_stale) return;
    Snapshot s = ReadLibrary(ctx, wanted);
    g_shown_library = library;
    g_shown_pick = wanted;
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

void fcip::editor::DrawArchetypeLibraryPanel() {
    {
        std::lock_guard lock(g_lock);
        if (g_snapshot.generation != g_view.generation) g_view = g_snapshot;
    }
    ImGui::TextUnformatted("ARCHETYPES");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("The archetype library, saved with the level for the archetypes its entities use");
    ImGui::PopStyleColor();
    ImGui::Separator();
    DrawList();
    DrawCreate();
    ImGui::Separator();
    ImGui::BeginChild("##archetype");
    DrawPickedArchetype();
    ImGui::EndChild();
}
