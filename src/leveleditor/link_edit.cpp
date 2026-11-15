// ige.xex, the solo level editor: event links between entities drawn in the view. With the Edit Object tool, Shift
// and a drag from the picked entity's marker to another entity's opens a window listing the links from one to the
// other, to remove or add; the events offered are each class's own (entity_events.h). Mission calls (target 0) take
// their ExtraParam from what the level has: its sequences, anchors and entities.
#include "leveleditor/link_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <set>

#include "leveleditor/entity_events.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/object_panel.h"

using namespace fcip::editor;

namespace {

constexpr float kPickPixels = 12.0f;
constexpr size_t kEventChars = 64;
constexpr size_t kParamChars = 256;
constexpr ImU32 kDragColour = IM_COL32(250, 220, 120, 255);

struct LinkDrag {
    bool on = false;
    uint32_t source = 0;
    float from[3] = {};
};
LinkDrag g_drag;

// the window a drag opened
struct LinkWindow {
    bool open = false;
    uint32_t source = 0;
    uint32_t target = 0;
    ImVec2 at;
    char source_event[kEventChars] = "";
    char target_event[kEventChars] = "";
};
LinkWindow g_window;

// the mission call row
char g_call[kEventChars] = "_Mission_PlaySequence";
char g_call_source[kEventChars] = "Enter";
char g_call_param[kParamChars] = "";

const ViewEntity* EntityNear(const View& v, ImVec2 m, uint32_t skip) {
    const ViewEntity* best = nullptr;
    float nearest = kPickPixels;
    for (const ViewEntity& e : *ViewEntities()) {
        ImVec2 at;
        if (e.id == skip || !Project(v, e.pos, at)) continue;
        float d = std::hypot(at.x - m.x, at.y - m.y);
        if (d < nearest) {
            nearest = d;
            best = &e;
        }
    }
    return best;
}

std::string NameOf(uint32_t entity_id) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.id == entity_id) return e.name;
    }
    return "entity " + std::to_string(entity_id);
}

// the class's first event, as a new link's default
void FirstOf(const std::vector<std::string>& events, char* text) {
    if (!events.empty()) std::snprintf(text, kEventChars, "%s", events.front().c_str());
}

void Open(uint32_t source, uint32_t target, ImVec2 at) {
    g_window = {};
    g_window.open = true;
    g_window.source = source;
    g_window.target = target;
    g_window.at = at;
    FirstOf(EventsOf(ClassOfEntity(source)), g_window.source_event);
    FirstOf(EventsOf(ClassOfEntity(target)), g_window.target_event);
}

std::vector<std::string> Sorted(std::set<std::string> names) { return {names.begin(), names.end()}; }

std::vector<std::string> EntityNames() {
    std::set<std::string> names;
    for (const ViewEntity& e : *ViewEntities()) {
        if (!e.name.empty()) names.insert(e.name);
    }
    return Sorted(names);
}

// a list of names the param takes, picking one sets or adds it
void ParamPicker(CallParam kind, const std::vector<std::string>& usual) {
    std::vector<std::string> names = usual;
    if (kind == CallParam::Sequence) names = SequenceNames();
    if (kind == CallParam::Anchor) names = AiObjectNames("AIAnchor");
    if (kind == CallParam::EntityAndBone || kind == CallParam::Names) {
        names = EntityNames();
        for (const std::string& tag : AiObjectNames("TagPoint")) names.push_back(tag);
    }
    ImGui::SameLine(0, 0);
    if (!ImGui::BeginCombo("##param_list", nullptr, ImGuiComboFlags_NoPreview)) return;
    for (const std::string& name : names) {
        if (!ImGui::Selectable(name.c_str())) continue;
        std::string param = g_call_param;
        // "entity,bone" keeps its bone, a list of names gets one more
        if (kind == CallParam::EntityAndBone) {
            size_t comma = param.find(',');
            param = name + (comma == std::string::npos ? ",Cam" : param.substr(comma));
        } else if (kind == CallParam::Names && !param.empty()) {
            param += "," + name;
        } else {
            param = name;
        }
        std::snprintf(g_call_param, kParamChars, "%s", param.c_str());
    }
    ImGui::EndCombo();
}

std::vector<std::string> CallNames() {
    std::set<std::string> calls;
    for (const MissionCall& c : kMissionCalls) calls.insert(c.event);
    for (const std::string& c : LevelMissionCalls()) calls.insert(c);
    return Sorted(calls);
}

void DrawWindowLinks() {
    std::vector<Link> links = LinksOf(g_window.source);
    std::string target = NameOf(g_window.target);
    for (size_t i = 0; i < links.size(); i++) {
        const Link& l = links[i];
        if (l.target != g_window.target) continue;
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) {
            std::vector<Link> kept = links;
            kept.erase(kept.begin() + std::ptrdiff_t(i));
            SetEntityLinks(g_window.source, kept);
        }
        ImGui::SameLine();
        ImGui::Text("On %s, %s gets %s", l.source.c_str(), target.c_str(), l.event.c_str());
        ImGui::PopID();
    }
    float em = ImGui::GetFontSize();
    std::vector<std::string> sends = EventsOf(ClassOfEntity(g_window.source));
    std::vector<std::string> takes = EventsOf(ClassOfEntity(g_window.target));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("On");
    ImGui::SameLine(em * 3);
    EventField("source", g_window.source_event, kEventChars, sends, em * 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("gets");
    ImGui::SameLine(em * 3);
    EventField("event", g_window.target_event, kEventChars, takes, em * 10);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_window.source_event[0] || !g_window.target_event[0]);
    if (ImGui::Button("Add")) {
        links.push_back({g_window.source_event, g_window.target, g_window.target_event, ""});
        SetEntityLinks(g_window.source, links);
    }
    ImGui::EndDisabled();
}

}  // namespace

namespace fcip::editor {

std::string ClassOfEntity(uint32_t entity_id) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.id == entity_id) return e.cls;
    }
    return {};
}

void EventField(const char* id, char* text, size_t bytes, const std::vector<std::string>& usual, float width) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##text", text, bytes);
    ImGui::SameLine(0, 0);
    ImGui::BeginDisabled(usual.empty());
    if (ImGui::BeginCombo("##usual", nullptr, ImGuiComboFlags_NoPreview)) {
        for (const std::string& e : usual) {
            if (ImGui::Selectable(e.c_str())) std::snprintf(text, bytes, "%s", e.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawMissionCallRow(uint32_t entity_id, const std::vector<Link>& links) {
    float em = ImGui::GetFontSize();
    float label_w = em * 4;
    ImGui::PushID("call");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("On");
    ImGui::SameLine(label_w);
    EventField("source", g_call_source, kEventChars, EventsOf(ClassOfEntity(entity_id)), em * 9);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("call");
    ImGui::SameLine(label_w);
    EventField("call", g_call, kEventChars, CallNames(), em * 12);
    const MissionCall* call = MissionCallOf(g_call);
    CallParam kind = call ? call->param : CallParam::Text;
    if (kind != CallParam::None) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("with");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(em * 12);
        ImGui::InputText("##param", g_call_param, kParamChars);
        ParamPicker(kind, call ? Words(call->usual) : std::vector<std::string>());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_call_source[0] || !g_call[0]);
    if (ImGui::Button("Add Call")) {
        std::vector<Link> added = links;
        added.push_back({g_call_source, 0, g_call, kind == CallParam::None ? "" : g_call_param});
        SetEntityLinks(entity_id, added);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

bool LinkMouse(ImVec2 mouse, bool pressed, bool down) {
    View v;
    if (g_drag.on) {
        if (down) return true;
        g_drag.on = false;
        const ViewEntity* target = ReadView(v) ? EntityNear(v, mouse, g_drag.source) : nullptr;
        if (target) Open(g_drag.source, target->id, mouse);
        return true;
    }
    uint32_t picked = SelectedEntityId();
    if (!pressed || !ImGui::GetIO().KeyShift || !picked || !ReadView(v)) return false;
    for (const ViewEntity& e : *ViewEntities()) {
        ImVec2 at;
        if (e.id != picked || !Project(v, e.pos, at)) continue;
        if (std::hypot(at.x - mouse.x, at.y - mouse.y) >= kPickPixels) return false;
        g_drag = {true, picked, {e.pos[0], e.pos[1], e.pos[2]}};
        return true;
    }
    return false;
}

void DrawLinkDrag(const View& v) {
    if (!g_drag.on) return;
    ImVec2 from;
    if (!Project(v, g_drag.from, from)) return;
    v.draw->AddLine(from, ImGui::GetIO().MousePos, kDragColour, 2.0f);
}

void DrawLinkWindow() {
    if (!g_window.open) return;
    ImGui::SetNextWindowPos(g_window.at, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0), ImGuiCond_Appearing);
    std::string title = NameOf(g_window.source) + " to " + NameOf(g_window.target) + "###links";
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(title.c_str(), &g_window.open, kFlags)) DrawWindowLinks();
    ImGui::End();
}

}  // namespace fcip::editor
