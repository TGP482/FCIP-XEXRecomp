// ige.xex, the solo level editor: links drawn in the view. With the Edit Object tool, Shift and a drag from the picked
// entity or mission object to another opens a window with what joins them: event links between entities, which an
// entity's class sends and takes (entity_events.h), and the AI's own links (ai_links.cpp). Mission calls (target 0)
// take their ExtraParam from what the level has: its sequences, anchors and entities.
#include "leveleditor/link_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <set>

#include "leveleditor/ai_links.h"
#include "leveleditor/ai_panel.h"
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
    Endpoint source;
};
LinkDrag g_drag;

// the window a drag opened
struct LinkWindow {
    bool open = false;
    Endpoint source;
    Endpoint target;
    ImVec2 at;
    char source_event[kEventChars] = "";
    char target_event[kEventChars] = "";
};
LinkWindow g_window;

// the mission call row
char g_call[kEventChars] = "_Mission_PlaySequence";
char g_call_source[kEventChars] = "Enter";
char g_call_param[kParamChars] = "";

Endpoint OfEntity(const ViewEntity& e) {
    return {e.id, 0, e.name, e.cls, {e.pos[0], e.pos[1], e.pos[2]}};
}

// a mission object is reached at its place, else its first point
bool OfObject(const AiObject& o, Endpoint& out) {
    if (!o.placed && o.points.empty()) return false;
    const float* at = o.placed ? o.pos : o.points.front().data();
    out = {0, o.node, o.name, o.type, {at[0], at[1], at[2]}};
    return true;
}

bool Same(const Endpoint& a, const Endpoint& b) { return a.entity_id == b.entity_id && a.object == b.object; }

// the entity or mission object nearest m on screen, other than skip
bool EndpointNear(const View& v, ImVec2 m, const Endpoint& skip, Endpoint& out) {
    float nearest = kPickPixels;
    auto consider = [&](const Endpoint& e) {
        ImVec2 at;
        if (Same(e, skip) || !Project(v, e.pos, at)) return;
        float d = std::hypot(at.x - m.x, at.y - m.y);
        if (d >= nearest) return;
        nearest = d;
        out = e;
    };
    for (const ViewEntity& e : *ViewEntities()) consider(OfEntity(e));
    for (const AiObject& o : ReadAiObjects()->objects) {
        Endpoint e;
        if (OfObject(o, e)) consider(e);
    }
    return nearest < kPickPixels;
}

// what the Edit Object tool or the AI tab has picked
bool PickedEndpoint(Endpoint& out) {
    if (uint32_t object = SelectedAiObject()) {
        for (const AiObject& o : ReadAiObjects()->objects) {
            if (o.node == object) return OfObject(o, out);
        }
        return false;
    }
    uint32_t id = SelectedEntityId();
    for (const ViewEntity& e : *ViewEntities()) {
        if (id && e.id == id) {
            out = OfEntity(e);
            return true;
        }
    }
    return false;
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

void Open(const Endpoint& source, const Endpoint& target, ImVec2 at) {
    g_window = {};
    g_window.open = true;
    g_window.source = source;
    g_window.target = target;
    g_window.at = at;
    FirstOf(EventsOf(source.kind), g_window.source_event);
    FirstOf(EventsOf(target.kind), g_window.target_event);
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

void DrawEventLinks(uint32_t source, uint32_t target_id) {
    std::vector<Link> links = LinksOf(source);
    std::string target = NameOf(target_id);
    for (size_t i = 0; i < links.size(); i++) {
        const Link& l = links[i];
        if (l.target != target_id) continue;
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) {
            std::vector<Link> kept = links;
            kept.erase(kept.begin() + std::ptrdiff_t(i));
            SetEntityLinks(source, kept);
        }
        ImGui::SameLine();
        ImGui::Text("On %s, %s gets %s", l.source.c_str(), target.c_str(), l.event.c_str());
        ImGui::PopID();
    }
    float em = ImGui::GetFontSize();
    std::vector<std::string> sends = EventsOf(ClassOfEntity(source));
    std::vector<std::string> takes = EventsOf(ClassOfEntity(target_id));
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
        links.push_back({g_window.source_event, target_id, g_window.target_event, ""});
        SetEntityLinks(source, links);
    }
    ImGui::EndDisabled();
}

void DrawWindow() {
    const Endpoint& from = g_window.source;
    const Endpoint& to = g_window.target;
    if (from.entity_id && to.entity_id) {
        ImGui::SeparatorText("Events");
        DrawEventLinks(from.entity_id, to.entity_id);
    }
    DrawAiLinks(from, to);
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
        Endpoint target;
        if (ReadView(v) && EndpointNear(v, mouse, g_drag.source, target)) Open(g_drag.source, target, mouse);
        return true;
    }
    Endpoint picked;
    if (!pressed || !ImGui::GetIO().KeyShift || !PickedEndpoint(picked) || !ReadView(v)) return false;
    ImVec2 at;
    if (!Project(v, picked.pos, at) || std::hypot(at.x - mouse.x, at.y - mouse.y) >= kPickPixels) return false;
    g_drag = {true, picked};
    return true;
}

void DrawLinkDrag(const View& v) {
    if (!g_drag.on) return;
    ImVec2 from;
    if (!Project(v, g_drag.source.pos, from)) return;
    v.draw->AddLine(from, ImGui::GetIO().MousePos, kDragColour, 2.0f);
}

void DrawLinkWindow() {
    if (!g_window.open) return;
    ImGui::SetNextWindowPos(g_window.at, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0), ImGuiCond_Appearing);
    std::string title = g_window.source.name + " to " + g_window.target.name + "###links";
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(title.c_str(), &g_window.open, kFlags)) DrawWindow();
    ImGui::End();
}

}  // namespace fcip::editor
