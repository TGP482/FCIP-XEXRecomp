// ige.xex, the solo level editor: the Formations tab edits the kept mission's AI formations and the templates they
// are made from. A template (FormationTemplatesLibrary, loaded by 821D43E0) is a list of slots, each with a role
// (the script's EAIFORMATIONSLOTROLE values 0 to 6), a leader flag, grenade use, priorities and the radii that lose
// or compromise the slot. A formation (Formations, loaded by 821D3FD0 as the mission loads) names its template, its
// two behaviours and the anchors its defend slots hold. Enemies join one by formFormationName. The AI system takes
// both once as the level loads, so edits show from the next time it opens.
#include "leveleditor/formations.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "kernel/memory.h"
#include "leveleditor/fields.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/node_edit.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kFormations = "Formations";
constexpr const char* kFormation = "formation";
constexpr const char* kTemplates = "FormationTemplatesLibrary";
constexpr const char* kTemplate = "formation_template";
constexpr const char* kSlot = "slot";
constexpr const char* kAnchor = "defend_slot_anchor";
constexpr const char* kName = "name";
constexpr const char* kTemplateName = "template_name";
constexpr const char* kAnchorName = "anchor_name";
constexpr const char* kId = "id";
constexpr const char* kType = "type";
constexpr const char* kRole = "role";
constexpr const char* kDefaultType = "2";  // every campaign template's

constexpr const char* kRoles[] = {"Attack", "Defend", "Hide Defend", "Flank Right", "Flank Left", "Cover Attack",
                                  "No Role"};
// a formation's flags, then a slot's values with the kind 821D43E0 reads them as
struct Key {
    const char* key;
    FieldKind kind;
};
constexpr Key kFormationKeys[] = {
    {"disorganized_when_leader_killed", FieldKind::Bool},
    {"user_support_fire", FieldKind::Bool},
};
constexpr Key kSlotKeys[] = {
    {"is_leader_slot", FieldKind::Bool},
    {"use_grenade", FieldKind::Bool},
    {"initial_priority", FieldKind::Float},
    {"increment_priority_for_death", FieldKind::Float},
    {"radius_for_losing_slot", FieldKind::Float},
    {"compromising_radius_front", FieldKind::Float},
    {"compromising_radius_behind", FieldKind::Float},
};

constexpr size_t kNameChars = 64;

struct Anchor {
    std::string id;
    std::string name;
};
struct Formation {
    std::vector<XmlAttribute> attributes;
    std::vector<Anchor> anchors;
};
struct Template {
    std::vector<XmlAttribute> attributes;
    std::vector<std::vector<XmlAttribute>> slots;
};
struct Snapshot {
    bool kept = false;
    std::vector<Formation> formations;
    std::vector<Template> templates;
    uint32_t generation = 0;
};
using Edit = std::function<void(PPCContext&)>;

std::mutex g_lock;
Snapshot g_snapshot;
std::vector<Edit> g_edits;
// editor thread
uint32_t g_shown = 0;
bool g_stale = false;
// window thread
Snapshot g_view;
char g_new_formation[kNameChars] = {};
char g_new_template[kNameChars] = {};

// Editor thread

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

uint32_t Named(uint32_t parent, const char* tag, const std::string& name) {
    for (uint32_t i = 0; parent && i < ChildCount(parent); i++) {
        uint32_t child = ChildAt(parent, i);
        if (Tag(child) == tag && XmlValue(child, kName) == name) return child;
    }
    return 0;
}

// the index'th child tagged tag
uint32_t NthTagged(uint32_t parent, const char* tag, int index) {
    for (uint32_t i = 0; parent && i < ChildCount(parent); i++) {
        uint32_t child = ChildAt(parent, i);
        if (Tag(child) == tag && index-- == 0) return child;
    }
    return 0;
}

Snapshot ReadFormations() {
    Snapshot s;
    uint32_t mission = KeptMission();
    s.kept = mission != 0;
    uint32_t formations = XmlChild(mission, kFormations);
    for (uint32_t i = 0; formations && i < ChildCount(formations); i++) {
        uint32_t node = ChildAt(formations, i);
        if (Tag(node) != kFormation) continue;
        Formation f{XmlAttributes(node), {}};
        for (uint32_t j = 0; j < ChildCount(node); j++) {
            uint32_t anchor = ChildAt(node, j);
            if (Tag(anchor) == kAnchor) f.anchors.push_back({XmlValue(anchor, kId), XmlValue(anchor, kAnchorName)});
        }
        s.formations.push_back(std::move(f));
    }
    uint32_t templates = XmlChild(mission, kTemplates);
    for (uint32_t i = 0; templates && i < ChildCount(templates); i++) {
        uint32_t node = ChildAt(templates, i);
        if (Tag(node) != kTemplate) continue;
        Template t{XmlAttributes(node), {}};
        for (uint32_t j = 0; j < ChildCount(node); j++) {
            if (Tag(ChildAt(node, j)) == kSlot) t.slots.push_back(XmlAttributes(ChildAt(node, j)));
        }
        s.templates.push_back(std::move(t));
    }
    return s;
}

void Reread(PPCContext&) { g_stale = true; }

// Edits queued by the panel, each one step of undo

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

// a change to the formation (or template) called name under the mission's parent tag
void QueueNamedEdit(const char* parent, const char* tag, std::string name, std::string label, NodeChange change) {
    Queue([=](PPCContext& ctx) {
        EditMissionNode(ctx, parent, label, [&](PPCContext& ctx, uint32_t list) {
            if (uint32_t node = Named(list, tag, name)) change(ctx, node);
        }, Reread);
    });
}

void SetFormationValue(const std::string& formation, std::string key, std::string value) {
    QueueNamedEdit(kFormations, kFormation, formation, "Set " + key + " of " + formation,
                   [=](PPCContext& ctx, uint32_t node) { SetXmlValue(ctx, node, key, value); });
}

void SetAnchorValue(const std::string& formation, int index, std::string key, std::string value) {
    QueueNamedEdit(kFormations, kFormation, formation, "Set a defend anchor of " + formation,
                   [=](PPCContext& ctx, uint32_t node) {
                       SetXmlValue(ctx, NthTagged(node, kAnchor, index), key, value);
                   });
}

void AddAnchor(const std::string& formation) {
    QueueNamedEdit(kFormations, kFormation, formation, "Add a defend anchor to " + formation,
                   [](PPCContext& ctx, uint32_t node) {
                       uint32_t anchor = AddXmlChild(ctx, node, kAnchor);
                       SetXmlValue(ctx, anchor, kId, "0");
                       SetXmlValue(ctx, anchor, kAnchorName, "");
                   });
}

void RemoveAnchor(const std::string& formation, int index) {
    QueueNamedEdit(kFormations, kFormation, formation, "Remove a defend anchor of " + formation,
                   [=](PPCContext& ctx, uint32_t node) {
                       RemoveXmlChild(ctx, node, NthTagged(node, kAnchor, index));
                   });
}

void SetTemplateValue(const std::string& name, std::string key, std::string value) {
    QueueNamedEdit(kTemplates, kTemplate, name, "Set " + key + " of " + name,
                   [=](PPCContext& ctx, uint32_t node) { SetXmlValue(ctx, node, key, value); });
}

void SetSlotValue(const std::string& name, int index, std::string key, std::string value) {
    QueueNamedEdit(kTemplates, kTemplate, name, "Set " + key + " of a slot of " + name,
                   [=](PPCContext& ctx, uint32_t node) {
                       SetXmlValue(ctx, NthTagged(node, kSlot, index), key, value);
                   });
}

// a slot with the next id and every value 0, role Attack, as the campaign's new slots
void AddSlot(const std::string& name, int id) {
    QueueNamedEdit(kTemplates, kTemplate, name, "Add a slot to " + name, [id](PPCContext& ctx, uint32_t node) {
        uint32_t slot = AddXmlChild(ctx, node, kSlot);
        for (const Key& k : kSlotKeys) SetXmlValue(ctx, slot, k.key, "0");
        SetXmlValue(ctx, slot, kRole, "0");
        SetXmlValue(ctx, slot, kId, std::to_string(id));
    });
}

void RemoveSlot(const std::string& name, int index) {
    QueueNamedEdit(kTemplates, kTemplate, name, "Remove a slot of " + name,
                   [=](PPCContext& ctx, uint32_t node) {
                       RemoveXmlChild(ctx, node, NthTagged(node, kSlot, index));
                   });
}

void Delete(const char* parent, const char* tag, std::string name) {
    Queue([=](PPCContext& ctx) {
        EditMissionNode(ctx, parent, "Delete " + name, [&](PPCContext& ctx, uint32_t list) {
            RemoveXmlChild(ctx, list, Named(list, tag, name));
        }, Reread);
    });
}

void CreateFormation(std::string name, std::string template_name) {
    Queue([=](PPCContext& ctx) {
        EditMissionNode(ctx, kFormations, "Create the formation " + name, [&](PPCContext& ctx, uint32_t list) {
            if (Named(list, kFormation, name)) return;
            uint32_t node = AddXmlChild(ctx, list, kFormation);
            SetXmlValue(ctx, node, kName, name);
            SetXmlValue(ctx, node, kTemplateName, template_name);
            for (const Key& k : kFormationKeys) SetXmlValue(ctx, node, k.key, "0");
        }, Reread);
    });
}

// a template with one leader slot
void CreateTemplate(std::string name) {
    Queue([=](PPCContext& ctx) {
        EditMissionNode(ctx, kTemplates, "Create the template " + name, [&](PPCContext& ctx, uint32_t list) {
            if (Named(list, kTemplate, name)) return;
            uint32_t node = AddXmlChild(ctx, list, kTemplate);
            SetXmlValue(ctx, node, kName, name);
            SetXmlValue(ctx, node, kType, kDefaultType);
            uint32_t slot = AddXmlChild(ctx, node, kSlot);
            for (const Key& k : kSlotKeys) SetXmlValue(ctx, slot, k.key, "0");
            SetXmlValue(ctx, slot, "is_leader_slot", "1");
            SetXmlValue(ctx, slot, kRole, "0");
            SetXmlValue(ctx, slot, kId, "0");
        }, Reread);
    });
}

// Panel, window thread

std::string Find(const std::vector<XmlAttribute>& attributes, const char* key) {
    for (const XmlAttribute& a : attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void Row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 13);
    ImGui::SetNextItemWidth(-1);
}

const Template* TemplateNamed(const std::string& name) {
    for (const Template& t : g_view.templates) {
        if (Find(t.attributes, kName) == name) return &t;
    }
    return nullptr;
}

// a combo of names; true with the one picked
bool NameCombo(const char* id, const std::string& current, const std::vector<std::string>& names, std::string& out) {
    if (!ImGui::BeginCombo(id, current.c_str())) return false;
    bool picked = false;
    for (const std::string& n : names) {
        if (!ImGui::Selectable(n.c_str(), n == current)) continue;
        out = n;
        picked = true;
    }
    ImGui::EndCombo();
    return picked;
}

std::vector<std::string> TemplateNames() {
    std::vector<std::string> names;
    for (const Template& t : g_view.templates) names.push_back(Find(t.attributes, kName));
    return names;
}

void DrawAnchors(const std::string& name, const Formation& f) {
    const Template* t = TemplateNamed(Find(f.attributes, kTemplateName));
    std::vector<std::string> ids;
    for (size_t i = 0; t && i < t->slots.size(); i++) ids.push_back(Find(t->slots[i], kId));
    std::vector<std::string> anchors = AiObjectNames("AIAnchor");
    float em = ImGui::GetFontSize();
    for (size_t i = 0; i < f.anchors.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) RemoveAnchor(name, int(i));
        ImGui::SameLine();
        ImGui::TextUnformatted("slot");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 4);
        std::string edited;
        if (NameCombo("##id", f.anchors[i].id, ids, edited)) SetAnchorValue(name, int(i), kId, edited);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (NameCombo("##anchor", f.anchors[i].name, anchors, edited)) {
            SetAnchorValue(name, int(i), kAnchorName, edited);
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add Defend Anchor")) AddAnchor(name);
}

void DrawFormation(const Formation& f) {
    std::string name = Find(f.attributes, kName);
    if (!ImGui::CollapsingHeader(name.c_str())) return;
    ImGui::PushID(name.c_str());
    Row(kTemplateName);
    std::string edited;
    if (NameCombo("##template", Find(f.attributes, kTemplateName), TemplateNames(), edited)) {
        SetFormationValue(name, kTemplateName, edited);
    }
    for (const Key& k : kFormationKeys) {
        Row(k.key);
        if (DrawField(k.key, k.kind, Find(f.attributes, k.key), edited)) SetFormationValue(name, k.key, edited);
    }
    DrawAnchors(name, f);
    if (ImGui::Button("Delete Formation")) Delete(kFormations, kFormation, name);
    ImGui::PopID();
}

void DrawSlot(const std::string& name, int index, const std::vector<XmlAttribute>& slot) {
    std::string title = "Slot " + Find(slot, kId);
    if (!ImGui::TreeNode(title.c_str())) return;
    std::string edited;
    Row(kRole);
    std::vector<std::string> roles(std::begin(kRoles), std::end(kRoles));
    if (DrawChoice(kRole, roles, Find(slot, kRole), edited)) SetSlotValue(name, index, kRole, edited);
    for (const Key& k : kSlotKeys) {
        Row(k.key);
        if (DrawField(k.key, k.kind, Find(slot, k.key), edited)) SetSlotValue(name, index, k.key, edited);
    }
    if (ImGui::SmallButton("Remove Slot")) RemoveSlot(name, index);
    ImGui::TreePop();
}

void DrawTemplate(const Template& t) {
    std::string name = Find(t.attributes, kName);
    if (!ImGui::CollapsingHeader(name.c_str())) return;
    ImGui::PushID(name.c_str());
    Row(kType);
    std::string edited;
    if (DrawField(kType, FieldKind::Int, Find(t.attributes, kType), edited)) SetTemplateValue(name, kType, edited);
    int next = 0;
    for (size_t i = 0; i < t.slots.size(); i++) {
        ImGui::PushID(int(i));
        DrawSlot(name, int(i), t.slots[i]);
        ImGui::PopID();
        next = std::max(next, std::atoi(Find(t.slots[i], kId).c_str()) + 1);
    }
    if (ImGui::Button("Add Slot")) AddSlot(name, next);
    ImGui::SameLine();
    if (ImGui::Button("Delete Template")) Delete(kTemplates, kTemplate, name);
    ImGui::PopID();
}

// a name field and a Create button, enabled for a name not yet taken
bool DrawCreate(const char* id, char* name, const std::vector<std::string>& taken) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##name", "new name", name, kNameChars);
    ImGui::SameLine();
    bool used = std::find(taken.begin(), taken.end(), std::string(name)) != taken.end();
    ImGui::BeginDisabled(!name[0] || used);
    bool create = ImGui::Button("Create");
    ImGui::EndDisabled();
    ImGui::PopID();
    return create;
}

}  // namespace

void fcip::editor::UpdateFormations(PPCContext& ctx) {
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    if (KeptMission() == g_shown && !g_stale) return;
    Snapshot s = ReadFormations();
    g_shown = KeptMission();
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

void fcip::editor::DrawFormationsPanel() {
    {
        std::lock_guard lock(g_lock);
        if (g_snapshot.generation != g_view.generation) g_view = g_snapshot;
    }
    ImGui::TextUnformatted("FORMATIONS");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Saved with the level, the AI takes them when the level opens");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!g_view.kept) {
        ImGui::TextUnformatted("Only a campaign or custom level keeps its mission's formations.");
        return;
    }
    ImGui::BeginChild("##formations");
    Heading("Formations");
    std::vector<std::string> formations;
    for (const Formation& f : g_view.formations) {
        DrawFormation(f);
        formations.push_back(Find(f.attributes, kName));
    }
    std::vector<std::string> templates = TemplateNames();
    ImGui::BeginDisabled(templates.empty());
    if (DrawCreate("formation", g_new_formation, formations)) {
        CreateFormation(g_new_formation, templates.front());
        g_new_formation[0] = 0;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    Heading("Formation Templates");
    for (const Template& t : g_view.templates) DrawTemplate(t);
    if (DrawCreate("template", g_new_template, templates)) {
        CreateTemplate(g_new_template);
        g_new_template[0] = 0;
    }
    ImGui::EndChild();
}
