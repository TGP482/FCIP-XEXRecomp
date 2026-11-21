// ige.xex, the solo level editor: the object panel in the editor's dock lists the script properties of the one object
// the Edit Object tool has selected, as the editor saves them, and writes edits back into the entity's tables. The
// game's own converters do the work: a table to XML as the editor saves, XML to a table as it loads. Below them are the
// object's event links, which another object (a door, a spawner, the level) gets when this one sends an event.
// Each value takes the widget its name asks for: a choice list for selX (the script's enumX list, not shown itself), a
// picker of the campaign's files or effects for file, model, texture and effect names, the archetype from the library.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/event_links.h"
#include "leveleditor/entity_events.h"
#include "leveleditor/fields.h"
#include "leveleditor/gizmo.h"
#include "leveleditor/link_edit.h"
#include "leveleditor/guest.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/mode.h"
#include "leveleditor/object_panel.h"
#include "leveleditor/picking.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// the tables the editor saves for an object: its Properties and an AI's per instance ones
constexpr const char* kTables[] = {"Properties", "PropertiesInstance"};

// Edit Object tool: the selected editor objects
constexpr uint32_t kToolSelectedFirst = 0x118;
constexpr uint32_t kToolSelectedLast = 0x11C;
constexpr uint32_t kObjectEntity = 0x2C;  // editor object
// IScriptObject methods
constexpr uint32_t kScriptGetValue = 0x44;
constexpr uint32_t kScriptRelease = 0x124;
// guest scratch: the converted node and table, then two texts
constexpr uint32_t kScratchNodeRef = 0x00;
constexpr uint32_t kScratchTableRef = 0x04;
constexpr uint32_t kScratchKey = 0x10;
constexpr uint32_t kScratchKeyBytes = 0x80;
constexpr uint32_t kScratchValue = kScratchKey + kScratchKeyBytes;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchValue + kScratchValueBytes;

constexpr size_t kValueChars = 256;
constexpr size_t kEventChars = 64;
struct Attribute {
    std::string key;
    std::string value;
};
// a table or one of its sub tables, found again by child indices from its table's root
struct Node {
    int table = 0;
    std::vector<int> path;
    std::string title;
    std::vector<Attribute> attributes;
};
// an entity a link can name
struct Target {
    uint32_t id = 0;
    std::string label;
};
struct Snapshot {
    uint32_t object = 0;  // editor object, 0 with nothing selected
    uint32_t entity_id = 0;
    std::string name;
    std::string archetype;
    std::vector<std::string> archetypes;  // of its class
    std::vector<Node> nodes;
    std::vector<fcip::editor::Link> links;
    std::vector<Target> targets;
    uint32_t generation = 0;
};
struct LinkEdit {
    uint32_t entity_id = 0;
    std::vector<fcip::editor::Link> links;
};
struct ArchetypeEdit {
    uint32_t entity_id = 0;
    std::string name;
};
// one value of an entity's table: its node by child indices from the table, then by sub table names
struct Edit {
    uint32_t entity_id = 0;
    int table = 0;  // of kTables, -1 for the one holding the key
    std::vector<int> path;
    std::vector<std::string> names;
    std::string key;
    std::string value;
    bool undoable = true;
};
// the entity's table and its XML, both referenced from the scratch
struct Converted {
    uint32_t table = 0;
    uint32_t node = 0;
};

std::mutex g_lock;
Snapshot g_snapshot;        // written on the editor's thread
std::vector<Edit> g_edits;  // queued by the panel
std::vector<LinkEdit> g_link_edits;
std::vector<ArchetypeEdit> g_archetype_edits;
uint32_t g_scratch = 0;
uint32_t g_shown = 0;  // the object the snapshot holds, editor thread only
bool g_stale = false;

// panel side, window thread
Snapshot g_view;
std::vector<std::array<char, kValueChars>> g_fields;
// the link being added
char g_new_source[kEventChars] = "Enter";
char g_new_event[kEventChars] = "Enable";
uint32_t g_new_target = 0;

// Editor thread

uint32_t WriteText(uint32_t at, const std::string& text, uint32_t bytes) {
    size_t n = std::min<size_t>(text.size(), bytes - 1);
    for (size_t i = 0; i < n; i++) W8(at + uint32_t(i), uint8_t(text[i]));
    W8(at + uint32_t(n), 0);
    return at;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

uint32_t SelectedObject(uint32_t tool) {
    uint32_t first = R32(tool + kToolSelectedFirst);
    if (!first || R32(tool + kToolSelectedLast) - first != 4) return 0;
    return R32(first);
}

std::string ClassOf(PPCContext& ctx, uint32_t entity) {
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    return cls ? ReadCString(cls) : std::string();
}

std::string LabelOf(PPCContext& ctx, uint32_t entity) {
    std::string cls = ClassOf(ctx, entity);
    std::string label = GuestString(entity + kEntityName);
    if (!cls.empty()) label += " (" + cls + ")";
    return label;
}

std::vector<Target> ReadTargets(PPCContext& ctx) {
    std::vector<Target> targets;
    ForEachEntity([&](uint32_t entity) { targets.push_back({R32(entity + kEntityId), LabelOf(ctx, entity)}); });
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.label < b.label; });
    return targets;
}

void Release(PPCContext& ctx, Converted& c) {
    if (c.node) CallGuest(ctx, Method(c.node, kNodeRelease), {c.node});
    if (c.table) CallGuest(ctx, Method(c.table, kScriptRelease), {c.table});
    c = {};
}

// the entity's table as XML, no node when the entity has no such table
Converted ToXml(PPCContext& ctx, uint32_t script, const char* table) {
    Converted c;
    W32(g_scratch + kScratchTableRef, 0);
    CallGuest(ctx, SYM_Script_NewTable, {g_scratch + kScratchTableRef, 1});
    c.table = R32(g_scratch + kScratchTableRef);
    if (!c.table) return c;
    bool found = CallGuest(ctx, Method(script, kScriptGetValue), {script, KeyText(table), c.table}) & 0xFF;
    if (!found) return c;
    c.node = uint32_t(CallGuest(ctx, SYM_Memory_Alloc, {0, 0, 0, kNodeBytes, 0}));
    if (!c.node) return c;
    CallGuest(ctx, SYM_XmlNode_Construct, {c.node, KeyText(table)});
    CallGuest(ctx, Method(c.node, kNodeAddRef), {c.node});
    W32(g_scratch + kScratchNodeRef, c.node);
    CallGuest(ctx, SYM_Xml_FromTable, {g_scratch + kScratchNodeRef, g_scratch + kScratchTableRef});
    return c;
}

void ReadNode(uint32_t node, const Node& at, std::vector<Node>& out) {
    Node n = at;
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        n.attributes.push_back({GuestString(a + kAttrKey), GuestString(a + kAttrValue)});
    }
    out.push_back(n);
    int index = 0;
    for (uint32_t c = R32(node + kNodeChildFirst); c && c < R32(node + kNodeChildLast); c += 4, index++) {
        uint32_t child = R32(c);
        Node sub{at.table, at.path, at.title + " / " + GuestString(child + kNodeTag), {}};
        sub.path.push_back(index);
        ReadNode(child, sub, out);
    }
}

bool IsEntity(uint32_t entity) {
    bool found = false;
    ForEachEntity([&](uint32_t e) { found = found || e == entity; });
    return found;
}

Snapshot Read(PPCContext& ctx, uint32_t object) {
    Snapshot s;
    s.object = object;
    // only an entity's own editor object holds it, zip line ends and mounted weapons hold other data there, and a
    // brush's object is keyed in the same map
    uint32_t entity = object ? R32(object + kObjectEntity) : 0;
    if (!entity || !IsEntity(entity) || EditorObjectFor(ctx, entity) != object) return s;
    uint32_t script = R32(entity + kEntityScript);
    if (!script) return s;
    s.entity_id = R32(entity + kEntityId);
    s.name = LabelOf(ctx, entity);
    s.archetype = ArchetypeOf(s.entity_id);
    s.archetypes = ArchetypesOfClass(ctx, ClassOf(ctx, entity));
    for (int t = 0; t < int(std::size(kTables)); t++) {
        Converted c = ToXml(ctx, script, kTables[t]);
        if (c.node) ReadNode(c.node, Node{t, {}, kTables[t], {}}, s.nodes);
        Release(ctx, c);
    }
    s.links = LinksOf(s.entity_id);
    s.targets = ReadTargets(ctx);
    return s;
}

uint32_t ChildNamed(uint32_t node, const std::string& tag) {
    for (uint32_t i = 0; i < ChildCount(node); i++) {
        if (GuestString(ChildAt(node, i) + kNodeTag) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t FindEdited(uint32_t table, const Edit& e) {
    uint32_t node = FindNode(table, e.path);
    for (size_t i = 0; node && i < e.names.size(); i++) node = ChildNamed(node, e.names[i]);
    return node;
}

// its script's OnReset then takes the new values (a trigger's box), as it does when play test ends; one step of undo
// the table whose node holds the key, Properties when none does
int TableOf(PPCContext& ctx, uint32_t script, const Edit& e) {
    for (int t = 0; t < int(std::size(kTables)); t++) {
        Converted c = ToXml(ctx, script, kTables[t]);
        uint32_t node = c.node ? FindEdited(c.node, e) : 0;
        bool holds = node && CallGuest(ctx, Method(node, kNodeGetAttr), {node, KeyText(e.key)}) != 0;
        Release(ctx, c);
        if (holds) return t;
    }
    return 0;
}

void Apply(PPCContext& ctx, const Edit& edit) {
    uint32_t entity = EntityOfId(edit.entity_id);
    uint32_t script = entity ? R32(entity + kEntityScript) : 0;
    if (!script) return;
    Edit e = edit;
    if (e.table < 0) e.table = TableOf(ctx, script, e);
    Converted c = ToXml(ctx, script, kTables[e.table]);
    uint32_t node = c.node ? FindEdited(c.node, e) : 0;
    std::string old;
    if (node) {
        uint32_t text = uint32_t(CallGuest(ctx, Method(node, kNodeGetAttr), {node, KeyText(e.key)}));
        if (text) old = ReadCString(text);
        CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(e.key), ValueText(e.value)});
        CallGuest(ctx, SYM_Xml_ToTable, {0, g_scratch + kScratchTableRef, g_scratch + kScratchNodeRef});
    }
    Release(ctx, c);
    if (!node) return;
    CallGuest(ctx, SYM_Entity_Reset, {entity});
    ForgetProperties(e.entity_id);
    NoteMapChanged();
    if (!e.undoable || old == e.value) return;
    Edit back = e;
    back.value = old;
    back.undoable = false;
    std::string label = "Set " + e.key + " of " + GuestString(entity + kEntityName);
    PushUndo(ctx, label, [back](PPCContext& ctx) { Apply(ctx, back); });
}

// the entity's links replaced, one step of undo
void ApplyLinks(PPCContext& ctx, const LinkEdit& e) {
    uint32_t entity = EntityOfId(e.entity_id);
    if (!entity) return;
    LinkEdit back{e.entity_id, LinksOf(e.entity_id)};
    SetLinks(ctx, entity, e.links);
    NoteMapChanged();
    std::string label = "Change the links of " + GuestString(entity + kEntityName);
    PushUndo(ctx, label, [back](PPCContext& ctx) {
        if (uint32_t entity = EntityOfId(back.entity_id)) SetLinks(ctx, entity, back.links);
    });
}

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

void Update(PPCContext& ctx, uint32_t object) {
    if (!EnsureScratch()) return;
    std::vector<Edit> edits;
    std::vector<LinkEdit> link_edits;
    std::vector<ArchetypeEdit> archetype_edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
        link_edits.swap(g_link_edits);
        archetype_edits.swap(g_archetype_edits);
    }
    for (const ArchetypeEdit& e : archetype_edits) {
        if (uint32_t entity = EntityOfId(e.entity_id)) SetArchetype(ctx, entity, e.name);
        g_stale = true;
    }
    for (const Edit& e : edits) {
        Apply(ctx, e);
        g_stale = true;
    }
    for (const LinkEdit& e : link_edits) {
        ApplyLinks(ctx, e);
        g_stale = true;
    }
    if (object == g_shown && !g_stale) return;
    Snapshot s = Read(ctx, object);
    g_shown = object;
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

// Panel, window thread

void TakeSnapshot() {
    std::lock_guard lock(g_lock);
    if (g_snapshot.generation == g_view.generation) return;
    g_view = g_snapshot;
    g_fields.clear();
    for (const Node& n : g_view.nodes) {
        for (const Attribute& a : n.attributes) {
            std::array<char, kValueChars> field{};
            a.value.copy(field.data(), kValueChars - 1);
            g_fields.push_back(field);
        }
    }
}

void Queue(const Node& n, const Attribute& a, const char* value) {
    std::lock_guard lock(g_lock);
    g_edits.push_back({g_view.entity_id, n.table, n.path, {}, a.key, value});
}

// an enemy's anchor fields (FirstAnchor, OnPlayerSeenAnchor1) name one of the level's anchors; AnchorGroupID does not
bool IsAnchorField(const std::string& key) {
    return key.find("Anchor") != std::string::npos && key.find("ID") == std::string::npos;
}

// a selX choice list's own table, or its copy saved beside selX: the script reads selX alone
bool IsChoiceList(const std::string& name) {
    return name.size() > 4 && name.compare(0, 4, "enum") == 0 && std::isupper(uint8_t(name[4]));
}

std::string LastTag(const Node& n) {
    size_t at = n.title.rfind(" / ");
    return at == std::string::npos ? n.title : n.title.substr(at + 3);
}

void Pick(const Node& n, const Attribute& a, char* text, const std::string& value) {
    std::snprintf(text, kValueChars, "%s", value.c_str());
    Queue(n, a, text);
}

void AnchorPicker(const Node& n, const Attribute& a, char* text) {
    ImGui::SameLine(0, 0);
    if (!ImGui::BeginCombo("##anchors", nullptr, ImGuiComboFlags_NoPreview)) return;
    if (ImGui::Selectable("(none)")) Pick(n, a, text, "");
    for (const std::string& name : AiObjectNames("AIAnchor")) {
        if (ImGui::Selectable(name.c_str())) Pick(n, a, text, name);
    }
    ImGui::EndCombo();
}

void DrawValue(const Node& n, const Attribute& a, char* text) {
    std::vector<std::string> choices = ChoicesOf(a.key);
    std::span<const char* const> items = PickerItemsOf(a.key, text);
    FieldKind kind = KindOfKey(a.key);
    bool anchor = IsAnchorField(a.key);
    ImGui::SetNextItemWidth(anchor ? -ImGui::GetFrameHeight() : -1);
    std::string edited;
    bool done = false;
    if (!choices.empty()) {
        done = DrawChoice("##choice", choices, text, edited);
    } else if (!items.empty()) {
        done = DrawPicker("##picker", items, text, edited);
    } else if (kind != FieldKind::Text) {
        done = DrawField("##typed", kind, text, edited);
    } else {
        ImGui::InputText("##value", text, kValueChars);
        if (ImGui::IsItemDeactivatedAfterEdit()) Queue(n, a, text);
    }
    if (done) Pick(n, a, text, edited);
    if (anchor) AnchorPicker(n, a, text);
}

void QueueArchetype(const std::string& name) {
    std::lock_guard lock(g_lock);
    g_archetype_edits.push_back({g_view.entity_id, name});
}

void DrawArchetype(float label_w) {
    if (g_view.archetypes.empty()) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Archetype");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    const char* preview = g_view.archetype.empty() ? "(none)" : g_view.archetype.c_str();
    if (!ImGui::BeginCombo("##archetype", preview, ImGuiComboFlags_HeightLarge)) return;
    for (const std::string& name : g_view.archetypes) {
        if (ImGui::Selectable(name.c_str(), name == g_view.archetype)) QueueArchetype(name);
    }
    ImGui::EndCombo();
}

void DrawFields() {
    float label_w = ImGui::GetFontSize() * 12;
    DrawArchetype(label_w);
    size_t field = 0;
    for (size_t i = 0; i < g_view.nodes.size(); i++) {
        const Node& n = g_view.nodes[i];
        if (IsChoiceList(LastTag(n))) {
            field += n.attributes.size();
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
        ImGui::TextUnformatted(n.title.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (const Attribute& a : n.attributes) {
            if (!IsChoiceList(a.key)) {
                ImGui::PushID(int(field));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a.key.c_str());
                ImGui::SameLine(label_w);
                DrawValue(n, a, g_fields[field].data());
                ImGui::PopID();
            }
            field++;
        }
    }
}

void QueueLinks(std::vector<fcip::editor::Link> links) {
    std::lock_guard lock(g_lock);
    g_link_edits.push_back({g_view.entity_id, std::move(links)});
}

std::string TargetLabel(uint32_t id) {
    if (id == 0) return "the mission";
    for (const Target& t : g_view.targets) {
        if (t.id == id) return t.label;
    }
    return "entity " + std::to_string(id);
}

void TargetField(float width) {
    ImGui::SetNextItemWidth(width);
    std::string preview = g_new_target ? TargetLabel(g_new_target) : "pick a target";
    if (!ImGui::BeginCombo("##target", preview.c_str())) return;
    for (const Target& t : g_view.targets) {
        if (t.id == g_view.entity_id) continue;
        if (ImGui::Selectable(t.label.c_str(), t.id == g_new_target)) g_new_target = t.id;
    }
    ImGui::EndCombo();
}

void DrawLinks() {
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
    ImGui::TextUnformatted("Events");
    ImGui::PopStyleColor();
    ImGui::Separator();
    for (size_t i = 0; i < g_view.links.size(); i++) {
        const fcip::editor::Link& l = g_view.links[i];
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) {
            std::vector<fcip::editor::Link> links = g_view.links;
            links.erase(links.begin() + std::ptrdiff_t(i));
            QueueLinks(std::move(links));
        }
        ImGui::SameLine();
        std::string target = TargetLabel(l.target);
        std::string param = l.param.empty() ? "" : " (" + l.param + ")";
        ImGui::Text("On %s, %s gets %s%s", l.source.c_str(), target.c_str(), l.event.c_str(), param.c_str());
        ImGui::PopID();
    }
    // a line each, so the dock can be narrow
    float em = ImGui::GetFontSize();
    float label_w = em * 3;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("On");
    ImGui::SameLine(label_w);
    std::vector<std::string> sends = EventsOf(ClassOfEntity(g_view.entity_id));
    EventField("source", g_new_source, kEventChars, sends, em * 9);
    ImGui::SetCursorPosX(label_w);
    TargetField(-1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("gets");
    ImGui::SameLine(label_w);
    EventField("event", g_new_event, kEventChars, EventsOf(ClassOfEntity(g_new_target)), em * 9);
    ImGui::SameLine();
    bool ready = g_new_target && g_new_source[0] && g_new_event[0];
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Add")) {
        std::vector<fcip::editor::Link> links = g_view.links;
        links.push_back({g_new_source, g_new_target, g_new_event, ""});
        QueueLinks(std::move(links));
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    DrawMissionCallRow(g_view.entity_id, g_view.links);
}

}  // namespace

std::vector<XmlAttribute> fcip::editor::ReadProperties(PPCContext& ctx, uint32_t entity) {
    std::vector<XmlAttribute> out;
    uint32_t script = R32(entity + kEntityScript);
    if (!script || !EnsureScratch()) return out;
    for (const char* table : kTables) {
        Converted c = ToXml(ctx, script, table);
        std::vector<Node> nodes;
        if (c.node) ReadNode(c.node, Node{}, nodes);
        Release(ctx, c);
        for (const Node& n : nodes) {
            // a sub table's title is " / Sub / Deeper", its keys become "Sub/Deeper/key"
            std::string prefix;
            for (size_t at = 0; (at = n.title.find(" / ", at)) != std::string::npos; at += 3) {
                size_t end = n.title.find(" / ", at + 3);
                prefix += n.title.substr(at + 3, end == std::string::npos ? std::string::npos : end - at - 3) + "/";
            }
            for (const Attribute& a : n.attributes) out.push_back({prefix + a.key, a.value});
        }
    }
    return out;
}

void fcip::editor::SetEntityLinks(uint32_t entity_id, std::vector<Link> links) {
    std::lock_guard lock(g_lock);
    g_link_edits.push_back({entity_id, std::move(links)});
}

void fcip::editor::SetEntityProperty(uint32_t entity_id, const std::string& key, const std::string& value) {
    // an AI's anchors are in PropertiesInstance
    Edit e{entity_id, -1, {}, {}, key, value};
    for (size_t slash; (slash = e.key.find('/')) != std::string::npos;) {
        e.names.push_back(e.key.substr(0, slash));
        e.key.erase(0, slash + 1);
    }
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(e));
}

uint32_t fcip::editor::SelectedEntityId() {
    std::lock_guard lock(g_lock);
    return g_snapshot.object ? g_snapshot.entity_id : 0;
}

void fcip::editor::DrawObjectPanel() {
    TakeSnapshot();
    ImGui::TextUnformatted("OBJECT PROPERTIES");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Enter or a click elsewhere applies a value");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!g_view.object) ImGui::TextUnformatted("Select one object with the Edit Object tool.");
    else ImGui::Text("Object: %s", g_view.name.c_str());
    ImGui::BeginChild("##fields");
    DrawFields();
    if (g_view.object) DrawLinks();
    ImGui::EndChild();
}

PPC_EXTERN_FUNC(__imp__sub_820DE570);
PPC_EXTERN_FUNC(__imp__sub_820DD258);

// the Edit Object tool's update, each frame while it runs; r3 the tool. A marker picked and a gizmo drag first
PPC_FUNC(sub_820DE570) {
    if (!fcip::editor::g_solo) return __imp__sub_820DE570(ctx, base);
    uint32_t tool = uint32_t(ctx.r[3]);
    ApplyPick(ctx, tool);
    ApplyGizmo(ctx);
    __imp__sub_820DE570(ctx, base);
    Update(ctx, SelectedObject(tool));
}

// leaving the Edit Object tool: nothing is selected for the panel
PPC_FUNC(sub_820DD258) {
    if (!fcip::editor::g_solo) return __imp__sub_820DD258(ctx, base);
    __imp__sub_820DD258(ctx, base);
    Update(ctx, 0);
}
