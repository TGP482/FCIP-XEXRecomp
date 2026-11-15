// the solo level editor's Sequences tab. A sequence picked shows a timeline of its nodes' keys to scrub, with a
// preview that puts its entities where the game's tracks have them at that time (sequences.cpp), and below it the
// sequence as moviedata.xml holds it: nodes, their tracks and keys, each value edited in place. Keys are added at the
// scrubbed time in time order; a position key takes its entity's place, a rotation key its facing. The picked
// sequence's position keys drag in the view on their own height.
#include "leveleditor/sequence_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "common/panel_colors.h"
#include "leveleditor/markers.h"
#include "leveleditor/sequences.h"

using namespace fcip::editor;

namespace {

constexpr size_t kValueChars = 256;
constexpr size_t kNameChars = 64;
constexpr float kListLines = 8.0f;
constexpr float kRowPixels = 14.0f;
constexpr float kGrabPixels = 8.0f;
constexpr float kHandleHalf = 4.0f;
constexpr float kHalfRadians = 3.14159265f / 360.0f;  // half a degree's radians
constexpr ImU32 kTickColour = IM_COL32(220, 120, 250, 255);
constexpr ImU32 kTimeColour = IM_COL32(255, 220, 60, 255);
constexpr ImU32 kHandleColour = IM_COL32(220, 120, 250, 255);

// what a track's ParamId animates (82529108 makes each kind)
struct TrackName {
    int param;
    const char* name;
};
constexpr TrackName kTrackNames[] = {
    {1, "Position"}, {2, "Rotation"}, {3, "Scale"}, {4, "Events"}, {7, "Sound"}, {8, "Sound"}, {9, "Sound"},
};
// a sound key's attributes, as the campaign's sound tracks have them
const std::vector<XmlAttribute> kSoundKey = {
    {"sound_event", "0"}, {"duration", "1"},         {"priority", "40"}, {"usePlayerEmitter", "0"},
    {"autorelease", "0"}, {"is_music", "0"},         {"is_scriptedevent_music", "0"},
};

std::shared_ptr<const Movie> g_movie;
uint32_t g_picked = 0;
float g_time = 0.0f;
bool g_preview = false;
bool g_playing = false;
const Movie* g_fields_movie = nullptr;
uint32_t g_fields_picked = 0;
std::vector<std::array<char, kValueChars>> g_fields;
char g_filter[kNameChars] = "";
char g_new_name[kNameChars] = "";

// a key being dragged in the view
struct KeyDrag {
    bool on = false;
    uint32_t sequence = 0;
    std::vector<int> path;
    float z = 0.0f;
    float pos[3] = {};
};
KeyDrag g_drag;

const Sequence* Picked() {
    for (const Sequence& s : *g_movie) {
        if (s.node == g_picked) return &s;
    }
    return nullptr;
}

void SyncFields(const Sequence* s) {
    if (g_fields_movie == g_movie.get() && g_fields_picked == g_picked) return;
    g_fields_movie = g_movie.get();
    g_fields_picked = g_picked;
    g_fields.clear();
    for (size_t i = 0; s && i < s->elements.size(); i++) {
        for (const XmlAttribute& a : s->elements[i].attributes) {
            std::array<char, kValueChars> field{};
            a.value.copy(field.data(), kValueChars - 1);
            g_fields.push_back(field);
        }
    }
}

std::string AttributeIn(const XmlElement& e, const char* key) {
    for (const XmlAttribute& a : e.attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

float FloatIn(const XmlElement& e, const char* key) { return std::strtof(AttributeIn(e, key).c_str(), nullptr); }

std::string TrackLabel(int param) {
    for (const TrackName& t : kTrackNames) {
        if (t.param == param) return t.name;
    }
    return "Track " + std::to_string(param);
}

const ViewEntity* EntityNamed(const std::string& name) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

std::string Format(float f) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", f);
    return text;
}

std::string FormatVector(const float* v, int count) {
    std::string text;
    for (int i = 0; i < count; i++) text += (i ? "," : "") + Format(v[i]);
    return text;
}

// the entity's facing as a rotation key, w x y z, from its angles (R = Rz Ry Rx)
std::string QuatOf(const float degrees[3]) {
    float cx = std::cos(degrees[0] * kHalfRadians);
    float sx = std::sin(degrees[0] * kHalfRadians);
    float cy = std::cos(degrees[1] * kHalfRadians);
    float sy = std::sin(degrees[1] * kHalfRadians);
    float cz = std::cos(degrees[2] * kHalfRadians);
    float sz = std::sin(degrees[2] * kHalfRadians);
    float q[4] = {cx * cy * cz + sx * sy * sz, sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                  cx * cy * sz - sx * sy * cz};
    return FormatVector(q, 4);
}

// a new key's attributes after its time, for the track's kind
std::vector<XmlAttribute> NewKey(int param, const std::string& entity_name) {
    const ViewEntity* e = EntityNamed(entity_name);
    std::vector<XmlAttribute> key = {{"time", Format(g_time)}};
    switch (param) {
        case 1: key.push_back({"value", e ? FormatVector(e->pos, 3) : "0,0,0"}); break;
        case 2: key.push_back({"value", e ? QuatOf(e->angles) : "1,0,0,0"}); break;
        case 3: key.push_back({"value", "1,1,1"}); break;
        case 4: key.push_back({"event", ""}); break;
        case 5: break;
        case 7:
        case 8:
        case 9: key.insert(key.end(), kSoundKey.begin(), kSoundKey.end()); break;
        default: key.push_back({"value", "0,0,0,0"}); break;
    }
    return key;
}

// the keys before the time, where a new one goes to keep them in order
int KeyIndex(const Sequence& s, const XmlElement& track) {
    int index = 0;
    for (const XmlElement& e : s.elements) {
        bool child = e.path.size() == track.path.size() + 1 &&
                     std::equal(track.path.begin(), track.path.end(), e.path.begin());
        if (child && FloatIn(e, "time") <= g_time) index++;
    }
    return index;
}

// the node an element sits under, its index among the Nodes
int NodeIndexOf(const XmlElement& e) { return e.path.size() >= 2 ? e.path[1] : -1; }

std::string NodeName(const Sequence& s, const XmlElement& e) {
    int i = NodeIndexOf(e);
    if (i < 0 || size_t(i) >= s.nodes.size()) return {};
    return s.nodes[size_t(i)].name;
}

std::string ElementLabel(const Sequence& s, const XmlElement& e) {
    if (e.tag == "Node") return "Node: " + NodeName(s, e);
    if (e.tag == "Track") return TrackLabel(std::atoi(AttributeIn(e, "ParamId").c_str()));
    if (e.tag == "Key") return "Key " + AttributeIn(e, "time") + "s";
    return e.tag;
}

void DrawSequenceList() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "name", g_filter, kNameChars);
    float height = ImGui::GetTextLineHeightWithSpacing() * kListLines;
    ImGui::BeginChild("##sequences", ImVec2(0, height), ImGuiChildFlags_Border);
    for (const Sequence& s : *g_movie) {
        if (g_filter[0] && s.name.find(g_filter) == std::string::npos) continue;
        std::string label = s.name + "##" + std::to_string(s.node);
        if (ImGui::Selectable(label.c_str(), s.node == g_picked)) {
            g_picked = s.node;
            g_time = s.start_time;
        }
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##new", "new sequence name", g_new_name, kNameChars);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_new_name[0]);
    if (ImGui::Button("Add Sequence")) AddSequence(g_new_name);
    ImGui::EndDisabled();
}

// a row per node with its keys as ticks; dragging across it scrubs
void DrawTimeline(const Sequence& s) {
    float span = std::max(s.end_time - s.start_time, 0.001f);
    float width = ImGui::GetContentRegionAvail().x;
    float height = kRowPixels * float(std::max<size_t>(s.nodes.size(), 1));
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    if (ImGui::IsItemActive()) {
        float x = (ImGui::GetIO().MousePos.x - at.x) / width;
        g_time = s.start_time + std::clamp(x, 0.0f, 1.0f) * span;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(fcip::kLine));
    for (const XmlElement& e : s.elements) {
        if (e.tag != "Key") continue;
        float x = at.x + (FloatIn(e, "time") - s.start_time) / span * width;
        float y = at.y + kRowPixels * (float(NodeIndexOf(e)) + 0.5f);
        draw->AddRectFilled(ImVec2(x - 2, y - 3), ImVec2(x + 2, y + 3), kTickColour);
    }
    float now = at.x + (g_time - s.start_time) / span * width;
    draw->AddLine(ImVec2(now, at.y), ImVec2(now, at.y + height), kTimeColour, 2.0f);
}

void DrawTransport(const Sequence& s) {
    if (g_playing) g_time += ImGui::GetIO().DeltaTime;
    if (g_time >= s.end_time) g_playing = false;
    g_time = std::clamp(g_time, s.start_time, s.end_time);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::SliderFloat("##time", &g_time, s.start_time, s.end_time, "%.2f s");
    ImGui::SameLine();
    if (ImGui::Button(g_playing ? "Pause" : "Play")) {
        g_playing = !g_playing;
        if (g_playing && g_time >= s.end_time) g_time = s.start_time;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Preview", &g_preview) && !g_preview) EndPreview();
    if (g_preview) PreviewSequence(s.name, g_time);
    DrawTimeline(s);
}

void DrawAddEntity(const Sequence& s) {
    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo("##add_entity", "Add an entity to the sequence")) return;
    std::vector<const ViewEntity*> entities;
    for (const ViewEntity& e : *ViewEntities()) {
        if (!e.name.empty()) entities.push_back(&e);
    }
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) { return a->name < b->name; });
    for (const ViewEntity* e : entities) {
        std::string label = e->name + "##" + std::to_string(e->id);
        if (ImGui::Selectable(label.c_str())) AddSequenceEntity(s.node, e->id);
    }
    ImGui::EndCombo();
}

void DrawElementButtons(const Sequence& s, const XmlElement& e) {
    if (e.tag == "Track") {
        ImGui::SameLine();
        std::string add = "Add Key at " + Format(g_time) + "s";
        int param = std::atoi(AttributeIn(e, "ParamId").c_str());
        if (ImGui::SmallButton(add.c_str())) {
            AddMovieChild(s.node, e.path, "Key", NewKey(param, NodeName(s, e)), KeyIndex(s, e));
        }
    }
    if (e.depth < 2) return;
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) RemoveMovieChild(s.node, e.path);
}

void DrawElements(const Sequence& s) {
    float em = ImGui::GetFontSize();
    size_t field = 0;
    for (size_t i = 0; i < s.elements.size(); i++) {
        const XmlElement& e = s.elements[i];
        float indent = em * float(e.depth);
        ImGui::PushID(int(i));
        if (indent > 0) ImGui::Indent(indent);
        ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
        ImGui::TextUnformatted(ElementLabel(s, e).c_str());
        ImGui::PopStyleColor();
        DrawElementButtons(s, e);
        for (const XmlAttribute& a : e.attributes) {
            ImGui::PushID(int(field));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.key.c_str());
            ImGui::SameLine(indent + em * 8);
            ImGui::SetNextItemWidth(-1);
            char* text = g_fields[field].data();
            ImGui::InputText("##value", text, kValueChars);
            if (ImGui::IsItemDeactivatedAfterEdit()) SetMovieAttribute(s.node, e.path, a.key, text);
            ImGui::PopID();
            field++;
        }
        if (indent > 0) ImGui::Unindent(indent);
        ImGui::PopID();
    }
}

void DrawPicked(const Sequence& s) {
    ImGui::Separator();
    ImGui::Text("%s", s.name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete Sequence")) RemoveSequence(s.node);
    DrawTransport(s);
    DrawAddEntity(s);
    ImGui::Separator();
    ImGui::BeginChild("##sequence_elements");
    DrawElements(s);
    ImGui::EndChild();
}

void DrawHandle(const View& v, const float p[3], ImU32 colour) {
    ImVec2 at;
    if (!Project(v, p, at)) return;
    ImVec2 min(at.x - kHandleHalf, at.y - kHandleHalf);
    ImVec2 max(at.x + kHandleHalf, at.y + kHandleHalf);
    v.draw->AddRectFilled(min, max, colour);
}

}  // namespace

namespace fcip::editor {

void DrawSequencePanel() {
    g_movie = CurrentMovie();
    ImGui::TextUnformatted("SEQUENCES");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Saved into the custom level's moviedata.xml");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!MovieKept()) {
        ImGui::TextUnformatted("Open a campaign or custom level to edit its sequences.");
        return;
    }
    DrawSequenceList();
    const Sequence* s = Picked();
    if (!s && g_preview) EndPreview();
    if (!s) g_preview = false;
    SyncFields(s);
    if (s) DrawPicked(*s);
}

bool SequenceMouse(ImVec2 mouse, bool pressed, bool down) {
    View v;
    if (g_drag.on) {
        if (down && ReadView(v)) MouseOnLevel(v, mouse, g_drag.z, g_drag.pos);
        if (down) return true;
        g_drag.on = false;
        SetMovieAttribute(g_drag.sequence, g_drag.path, "value", FormatVector(g_drag.pos, 3));
        return true;
    }
    g_movie = CurrentMovie();
    const Sequence* s = Picked();
    if (!pressed || !s || !ReadView(v)) return false;
    for (const MovieNode& n : s->nodes) {
        for (const Key& k : n.path) {
            ImVec2 at;
            if (!Project(v, k.pos, at) || std::hypot(at.x - mouse.x, at.y - mouse.y) >= kGrabPixels) continue;
            g_drag = {true, s->node, k.path, k.pos[2], {k.pos[0], k.pos[1], k.pos[2]}};
            return true;
        }
    }
    return false;
}

void DrawSequenceKeys(const View& v) {
    if (g_drag.on) {
        DrawHandle(v, g_drag.pos, kTimeColour);
        return;
    }
    g_movie = CurrentMovie();
    const Sequence* s = Picked();
    for (size_t i = 0; s && i < s->nodes.size(); i++) {
        for (const Key& k : s->nodes[i].path) DrawHandle(v, k.pos, kHandleColour);
    }
}

}  // namespace fcip::editor
