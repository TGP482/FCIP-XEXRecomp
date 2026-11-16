// ige.xex, the solo level editor: entities keep their own archetypes through a save. A campaign entity names its
// archetype (<Properties tplCreatureType=.../>) and overrides it (<LocalProperties>); the engine merges the two as it
// loads (8229F090). The editor's save (821043A0) finds an archetype by the entity's class in its catalogue instead,
// so a campaign entity was written with the whole table and no archetype, or with a catalogue archetype and no
// overrides. Each entity's own is kept here as it loads and written back with its current table as the overrides, and
// the saved mission's EntityLibrary gets every archetype its entities name, campaign enemies from other levels too.
// The Properties tab picks an entity's archetype from the library's entries of its class (EntityPrototype Class).
#include "leveleditor/archetypes.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/markers.h"
#include "leveleditor/mode.h"
#include "leveleditor/picking.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kProperties = "Properties";
constexpr const char* kLocalProperties = "LocalProperties";
constexpr const char* kArchetypeKey = "tplCreatureType";
constexpr const char* kLibrary = "EntityLibrary";
constexpr const char* kPrototype = "EntityPrototype";
constexpr const char* kObjects = "Objects";
constexpr const char* kName = "Name";
constexpr const char* kClass = "Class";

// IScriptObject methods
constexpr uint32_t kScriptGetValue = 0x44;
constexpr uint32_t kScriptRelease = 0x124;
// std::string
constexpr uint32_t kStrText = 0x04;
constexpr uint32_t kStrLength = 0x14;
constexpr uint32_t kStrCapacity = 0x18;
constexpr uint32_t kStrEmptyCapacity = 0x0F;

// guest scratch: refs, a std::string, then two texts
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchTableRef = 0x04;
constexpr uint32_t kScratchString = 0x10;
constexpr uint32_t kScratchKey = 0x30;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = kScratchKey + kScratchKeyBytes;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchValue + kScratchValueBytes;

// an entity's archetype and the class it was made with; an entity id given out again names another class
struct Archetype {
    std::string name;
    std::string cls;
};

std::map<uint32_t, Archetype> g_archetypes;  // by entity id
uint32_t g_scratch = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

uint32_t ChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        if (Tag(ChildAt(node, i)) == tag) return ChildAt(node, i);
    }
    return 0;
}

// the node's only attribute when it is key's, else empty: the engine takes the archetype only then
std::string SoleAttribute(uint32_t node, const char* key) {
    uint32_t first = R32(node + kNodeAttrFirst);
    if (!first || R32(node + kNodeAttrLast) - first != kAttrBytes) return {};
    return GuestString(first + kAttrKey) == key ? GuestString(first + kAttrValue) : std::string();
}

std::string NodeAttribute(uint32_t node, const char* key) {
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        if (GuestString(a + kAttrKey) == key) return GuestString(a + kAttrValue);
    }
    return {};
}

std::string ClassOf(PPCContext& ctx, uint32_t entity) {
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    return cls ? ReadCString(cls) : std::string();
}

void SetAttr(PPCContext& ctx, uint32_t node, const char* key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

// a new child of parent, which keeps it
uint32_t NewChild(PPCContext& ctx, uint32_t parent, const char* tag) {
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

// the entity's script Properties table, its reference in the scratch; 0 when it has none, else released by the caller
uint32_t PropertiesTable(PPCContext& ctx, uint32_t entity) {
    uint32_t script = R32(entity + kEntityScript);
    if (!script) return 0;
    W32(g_scratch + kScratchTableRef, 0);
    CallGuest(ctx, SYM_Script_NewTable, {g_scratch + kScratchTableRef, 1});
    uint32_t table = R32(g_scratch + kScratchTableRef);
    if (!table) return 0;
    bool found = CallGuest(ctx, Method(script, kScriptGetValue), {script, KeyText(kProperties), table}) & 0xFF;
    if (found) return table;
    CallGuest(ctx, Method(table, kScriptRelease), {table});
    return 0;
}

// the entity's script Properties table into node, as the save writes tables
void WriteTable(PPCContext& ctx, uint32_t entity, uint32_t node) {
    uint32_t table = PropertiesTable(ctx, entity);
    if (!table) return;
    W32(g_scratch + kScratchRef, node);
    CallGuest(ctx, SYM_Xml_FromTable, {g_scratch + kScratchRef, g_scratch + kScratchTableRef});
    CallGuest(ctx, Method(table, kScriptRelease), {table});
}

// node's values into the entity's Properties, each key its table already has, as the engine loads properties
void ReadTable(PPCContext& ctx, uint32_t entity, uint32_t node) {
    uint32_t table = PropertiesTable(ctx, entity);
    if (!table) return;
    W32(g_scratch + kScratchRef, node);
    CallGuest(ctx, SYM_Xml_ToTable, {0, g_scratch + kScratchTableRef, g_scratch + kScratchRef});
    CallGuest(ctx, Method(table, kScriptRelease), {table});
}

// a new node holding the entity's Properties, referenced once, 0 when it could not be made
uint32_t CopyTable(PPCContext& ctx, uint32_t entity) {
    uint32_t node = uint32_t(CallGuest(ctx, SYM_Memory_Alloc, {0, 0, 0, kNodeBytes, 0}));
    if (!node) return 0;
    CallGuest(ctx, SYM_XmlNode_Construct, {node, KeyText(kProperties)});
    CallGuest(ctx, Method(node, kNodeAddRef), {node});
    WriteTable(ctx, entity, node);
    return node;
}

// the entity's script takes its new values, the view reads them again
void NoteChanged(PPCContext& ctx, uint32_t entity) {
    CallGuest(ctx, SYM_Entity_Reset, {entity});
    ForgetProperties(R32(entity + kEntityId));
    NoteMapChanged();
}

// the editor's library entry for an archetype, referenced once, 0 when it has none
uint32_t FindPrototype(PPCContext& ctx, const std::string& name) {
    uint32_t str = g_scratch + kScratchString;
    W32(str + kStrText, 0);
    W32(str + kStrLength, 0);
    W32(str + kStrCapacity, kStrEmptyCapacity);
    CallGuest(ctx, SYM_String_Assign, {str, ValueText(name)});
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, SYM_Library_Find, {g_scratch + kScratchRef, uint32_t(CallGuest(ctx, SYM_Library_Get, {})), str});
    CallGuest(ctx, SYM_String_Destroy, {str});
    return R32(g_scratch + kScratchRef);
}

// a copy of the editor's entry under the mission's library, named in full as the mission's own are
void AddPrototype(PPCContext& ctx, uint32_t library, const std::string& name) {
    uint32_t prototype = FindPrototype(ctx, name);
    if (!prototype) return;
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(prototype, kNodeClone), {g_scratch + kScratchRef, prototype});
    uint32_t copy = R32(g_scratch + kScratchRef);
    CallGuest(ctx, Method(prototype, kNodeRelease), {prototype});
    if (!copy) return;
    SetAttr(ctx, copy, kName, name);
    W32(g_scratch + kScratchRef, copy);
    CallGuest(ctx, Method(library, kNodeAddChild), {library, g_scratch + kScratchRef});
}

}  // namespace

namespace fcip::editor {

void ForgetArchetypes() { g_archetypes.clear(); }

std::string ArchetypeOf(uint32_t entity_id) {
    auto found = g_archetypes.find(entity_id);
    return found == g_archetypes.end() ? std::string() : found->second.name;
}

std::vector<uint32_t> EntitiesOfArchetype(const std::string& name) {
    std::vector<uint32_t> ids;
    for (const auto& [id, archetype] : g_archetypes) {
        if (archetype.name == name) ids.push_back(id);
    }
    return ids;
}

void WritePropertiesTo(PPCContext& ctx, uint32_t entity, uint32_t node) {
    if (EnsureScratch()) WriteTable(ctx, entity, node);
}

std::vector<std::string> ArchetypesOfClass(PPCContext& ctx, const std::string& cls) {
    std::vector<std::string> names;
    uint32_t library = uint32_t(CallGuest(ctx, SYM_Library_Get, {}));
    uint32_t root = library ? R32(library) : 0;
    for (uint32_t i = 0; root && i < ChildCount(root); i++) {
        uint32_t prototype = ChildAt(root, i);
        if (NodeAttribute(prototype, kClass) == cls) names.push_back(NodeAttribute(prototype, kName));
    }
    std::sort(names.begin(), names.end());
    return names;
}

void SetArchetype(PPCContext& ctx, uint32_t entity, const std::string& name) {
    if (!EnsureScratch()) return;
    uint32_t prototype = FindPrototype(ctx, name);
    uint32_t properties = prototype ? ChildTagged(prototype, kProperties) : 0;
    uint32_t before = properties ? CopyTable(ctx, entity) : 0;
    if (before) {
        uint32_t id = R32(entity + kEntityId);
        auto found = g_archetypes.find(id);
        Archetype old = found == g_archetypes.end() ? Archetype{} : found->second;
        ReadTable(ctx, entity, properties);
        g_archetypes[id] = {name, ClassOf(ctx, entity)};
        NoteChanged(ctx, entity);
        auto release = [before](PPCContext& ctx) { CallGuest(ctx, Method(before, kNodeRelease), {before}); };
        std::string label = "Set the archetype of " + GuestString(entity + kEntityName);
        PushUndo(ctx, label, [id, old, before, release](PPCContext& ctx) {
            if (uint32_t entity = EntityOfId(id); entity && EnsureScratch()) {
                ReadTable(ctx, entity, before);
                if (old.name.empty()) g_archetypes.erase(id);
                else g_archetypes[id] = old;
                NoteChanged(ctx, entity);
            }
            release(ctx);
        }, release);
    }
    if (prototype) CallGuest(ctx, Method(prototype, kNodeRelease), {prototype});
}

void SaveArchetype(PPCContext& ctx, uint32_t entity, uint32_t node) {
    auto found = g_archetypes.find(R32(entity + kEntityId));
    if (found == g_archetypes.end() || !EnsureScratch()) return;
    if (found->second.cls != ClassOf(ctx, entity)) return;
    while (uint32_t written = ChildTagged(node, kProperties)) RemoveChild(ctx, node, written);
    if (uint32_t properties = NewChild(ctx, node, kProperties)) SetAttr(ctx, properties, kArchetypeKey, found->second.name);
    if (uint32_t local = NewChild(ctx, node, kLocalProperties)) WriteTable(ctx, entity, local);
}

void AddSavedPrototypes(PPCContext& ctx, uint32_t mission) {
    uint32_t objects = ChildTagged(mission, kObjects);
    if (!objects || !EnsureScratch()) return;
    uint32_t library = ChildTagged(mission, kLibrary);
    if (!library) library = NewChild(ctx, mission, kLibrary);
    if (!library) return;
    std::set<std::string> kept;
    for (uint32_t i = 0; i < ChildCount(library); i++) {
        uint32_t prototype = ChildAt(library, i);
        if (Tag(prototype) == kPrototype) kept.insert(NodeAttribute(prototype, kName));
    }
    for (uint32_t i = 0; i < ChildCount(objects); i++) {
        uint32_t properties = ChildTagged(ChildAt(objects, i), kProperties);
        std::string name = properties ? SoleAttribute(properties, kArchetypeKey) : std::string();
        if (!name.empty() && kept.insert(name).second) AddPrototype(ctx, library, name);
    }
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_8229F090);

// an entity's properties load (r4 the entity, r5 its node's XmlNodeRef): its archetype is kept by its id
PPC_FUNC(sub_8229F090) {
    if (!fcip::editor::g_solo) return __imp__sub_8229F090(ctx, base);
    uint32_t entity = uint32_t(ctx.r[4]);
    uint32_t node = R32(uint32_t(ctx.r[5]));
    uint32_t properties = node ? ChildTagged(node, kProperties) : 0;
    std::string name = properties ? SoleAttribute(properties, kArchetypeKey) : std::string();
    uint32_t id = R32(entity + kEntityId);
    if (name.empty()) {
        g_archetypes.erase(id);
    } else if (EnsureScratch()) {
        g_archetypes[id] = {name, ClassOf(ctx, entity)};
    }
    __imp__sub_8229F090(ctx, base);
}
