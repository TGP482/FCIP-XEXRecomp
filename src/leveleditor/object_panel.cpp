// ige.xex, the solo level editor: the object panel (F4) lists the script properties of the one object the Edit
// Object tool has selected, as the editor saves them, and writes edits back into the entity's tables. The game's own
// converters do the work: a table to XML as the editor saves, XML to a table as it loads. Below them are the object's
// event links, which another object (a door, a spawner, the level) gets when this one sends an event.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <imgui.h>

#include "common/menu.h"
#include "common/panel_colors.h"
#include "kernel/memory.h"
#include "leveleditor/event_links.h"
#include "leveleditor/guest.h"
#include "leveleditor/markers.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr uintptr_t kPanelKey = VK_F4;
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
// the events the campaign's links use most, offered beside the free text
constexpr const char* kSourceEvents[] = {"Enter", "Leave", "Spawn", "OutputTrigger", "OnDeath", "Die", "Explode",
                                         "Use", "StopUse", "PlayerEntered", "OnSpawnedEntityDied"};
constexpr const char* kTargetEvents[] = {"Enable", "Disable", "Activate", "DeActivate", "InputTrigger", "Spawn",
                                         "SpawnMax", "SwitchToGenericJob", "GoPath", "Play", "Stop", "Explode",
                                         "Die", "SetCurrent", "DisplayText", "_Mission_Failed"};

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
    std::vector<Node> nodes;
    std::vector<fcip::editor::Link> links;
    std::vector<Target> targets;
    uint32_t generation = 0;
};
struct LinkEdit {
    uint32_t object = 0;
    std::vector<fcip::editor::Link> links;
};
struct Edit {
    uint32_t object = 0;
    int table = 0;
    std::vector<int> path;
    std::string key;
    std::string value;
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
uint32_t g_scratch = 0;
uint32_t g_shown = 0;  // the object the snapshot holds, editor thread only
bool g_stale = false;

// panel side, window thread
bool g_open = false;
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

std::string LabelOf(PPCContext& ctx, uint32_t entity) {
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    std::string label = GuestString(entity + kEntityName);
    if (cls) label += " (" + ReadCString(cls) + ")";
    return label;
}

std::vector<Target> ReadTargets(PPCContext& ctx) {
    std::vector<Target> targets;
    ForEachEntity([&](uint32_t entity) { targets.push_back({R32(entity + kEntityId), LabelOf(ctx, entity)}); });
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.label < b.label; });
    return targets;
}

uint32_t ScriptOf(uint32_t object) {
    uint32_t entity = R32(object + kObjectEntity);
    return entity ? R32(entity + kEntityScript) : 0;
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

uint32_t FindNode(uint32_t root, const std::vector<int>& path) {
    uint32_t node = root;
    for (int index : path) {
        uint32_t first = R32(node + kNodeChildFirst);
        uint32_t count = first ? (R32(node + kNodeChildLast) - first) / 4 : 0;
        if (uint32_t(index) >= count) return 0;
        node = R32(first + uint32_t(index) * 4);
    }
    return node;
}

Snapshot Read(PPCContext& ctx, uint32_t object) {
    Snapshot s;
    s.object = object;
    uint32_t script = object ? ScriptOf(object) : 0;
    if (!script) return s;
    uint32_t entity = R32(object + kObjectEntity);
    s.entity_id = R32(entity + kEntityId);
    s.name = LabelOf(ctx, entity);
    for (int t = 0; t < int(std::size(kTables)); t++) {
        Converted c = ToXml(ctx, script, kTables[t]);
        if (c.node) ReadNode(c.node, Node{t, {}, kTables[t], {}}, s.nodes);
        Release(ctx, c);
    }
    s.links = LinksOf(s.entity_id);
    s.targets = ReadTargets(ctx);
    return s;
}

void Apply(PPCContext& ctx, const Edit& e) {
    uint32_t script = ScriptOf(e.object);
    if (!script) return;
    Converted c = ToXml(ctx, script, kTables[e.table]);
    uint32_t node = c.node ? FindNode(c.node, e.path) : 0;
    if (node) {
        CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(e.key), ValueText(e.value)});
        CallGuest(ctx, SYM_Xml_ToTable, {0, g_scratch + kScratchTableRef, g_scratch + kScratchNodeRef});
    }
    Release(ctx, c);
}

void Update(PPCContext& ctx, uint32_t object) {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    if (!g_scratch) return;
    std::vector<Edit> edits;
    std::vector<LinkEdit> link_edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
        link_edits.swap(g_link_edits);
    }
    for (const Edit& e : edits) {
        if (e.object != object) continue;
        Apply(ctx, e);
        g_stale = true;
    }
    for (const LinkEdit& e : link_edits) {
        if (e.object != object) continue;
        SetLinks(ctx, R32(object + kObjectEntity), e.links);
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
    g_edits.push_back({g_view.object, n.table, n.path, a.key, value});
}

void DrawFields() {
    float label_w = ImGui::GetFontSize() * 12;
    size_t field = 0;
    for (size_t i = 0; i < g_view.nodes.size(); i++) {
        const Node& n = g_view.nodes[i];
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
        ImGui::TextUnformatted(n.title.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (const Attribute& a : n.attributes) {
            ImGui::PushID(int(field));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.key.c_str());
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-1);
            char* text = g_fields[field].data();
            ImGui::InputText("##value", text, kValueChars);
            if (ImGui::IsItemDeactivatedAfterEdit()) Queue(n, a, text);
            ImGui::PopID();
            field++;
        }
    }
}

void QueueLinks(std::vector<fcip::editor::Link> links) {
    std::lock_guard lock(g_lock);
    g_link_edits.push_back({g_view.object, std::move(links)});
}

std::string TargetLabel(uint32_t id) {
    for (const Target& t : g_view.targets) {
        if (t.id == id) return t.label;
    }
    return "entity " + std::to_string(id);
}

// a text field with the usual values in a list beside it
template <size_t N>
void EventField(const char* id, char (&text)[kEventChars], const char* const (&usual)[N], float width) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##text", text, kEventChars);
    ImGui::SameLine(0, 0);
    if (ImGui::BeginCombo("##usual", nullptr, ImGuiComboFlags_NoPreview)) {
        for (const char* e : usual) {
            if (ImGui::Selectable(e)) std::snprintf(text, kEventChars, "%s", e);
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
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
        ImGui::Text("On %s, %s gets %s", l.source.c_str(), target.c_str(), l.event.c_str());
        ImGui::PopID();
    }
    float em = ImGui::GetFontSize();
    ImGui::TextUnformatted("On");
    ImGui::SameLine();
    EventField("source", g_new_source, kSourceEvents, em * 7);
    ImGui::SameLine();
    TargetField(em * 12);
    ImGui::SameLine();
    ImGui::TextUnformatted("gets");
    ImGui::SameLine();
    EventField("event", g_new_event, kTargetEvents, em * 7);
    ImGui::SameLine();
    bool ready = g_new_target && g_new_source[0] && g_new_event[0];
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Add")) {
        std::vector<fcip::editor::Link> links = g_view.links;
        links.push_back({g_new_source, g_new_target, g_new_event, ""});
        QueueLinks(std::move(links));
    }
    ImGui::EndDisabled();
}

void DrawPanel() {
    if (!g_open) return;
    TakeSnapshot();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - em, vp->Pos.y + em), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(em * 36, vp->Size.y - em * 2), ImGuiCond_Always);
    for (const auto& [col, color] : fcip::kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##object_panel", nullptr, kFlags)) {
        ImGui::TextUnformatted("OBJECT PROPERTIES");
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
        ImGui::TextUnformatted("Enter or a click elsewhere applies a value, F4 or Escape closes");
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (!g_view.object) ImGui::TextUnformatted("Select one object with the Edit Object tool.");
        else ImGui::Text("Object: %s", g_view.name.c_str());
        ImGui::BeginChild("##fields");
        DrawFields();
        if (g_view.object) DrawLinks();
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(int(std::size(fcip::kPanelColors)));
}

bool PanelKey(uint32_t, uintptr_t wp) {
    if (!fcip::editor::g_solo) return false;
    bool toggle = wp == kPanelKey;
    bool close = wp == VK_ESCAPE && g_open;
    if (!toggle && !close) return false;
    g_open = toggle ? !g_open : false;
    fcip::g_editor_panel_open = g_open;
    return true;
}

// the markers under the panel
void DrawEditorUi() {
    if (!fcip::editor::g_solo) return;
    DrawMarkers();
    DrawPanel();
}

const bool g_registered = (fcip::g_editor_panel = DrawEditorUi, fcip::g_editor_panel_key = PanelKey, true);

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_820DE570);
PPC_EXTERN_FUNC(__imp__sub_820DD258);

// the Edit Object tool's update, each frame while it runs; r3 the tool
PPC_FUNC(sub_820DE570) {
    if (!fcip::editor::g_solo) return __imp__sub_820DE570(ctx, base);
    uint32_t tool = uint32_t(ctx.r[3]);
    __imp__sub_820DE570(ctx, base);
    Update(ctx, SelectedObject(tool));
    UpdateMarkers(ctx);
}

// leaving the Edit Object tool: nothing is selected for the panel
PPC_FUNC(sub_820DD258) {
    if (!fcip::editor::g_solo) return __imp__sub_820DD258(ctx, base);
    __imp__sub_820DD258(ctx, base);
    Update(ctx, 0);
}
