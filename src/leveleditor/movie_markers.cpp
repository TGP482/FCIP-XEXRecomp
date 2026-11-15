// ige.xex, the solo level editor: the level's cutscenes and scripted sequences in the view (sequences.cpp keeps them).
// Each moved entity's keys are joined in order, from where the sequence first places it, with the events it gets
// near the camera. A trigger's PlaySequence and _Mission_PlaySequence point at where the sequence starts.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "leveleditor/markers.h"
#include "leveleditor/sequences.h"

using namespace fcip::editor;

namespace {

constexpr ImU32 kPathColour = IM_COL32(220, 120, 250, 255);
constexpr ImU32 kKeyColour = IM_COL32(220, 120, 250, 150);

// a node without keys is labelled where its entity stands
void DrawNode(const View& v, const Sequence& s, const MovieNode& n, const float* entity) {
    for (size_t i = 0; i < n.path.size(); i++) {
        Square(v, n.path[i].pos, kKeyColour);
        if (i > 0) Arrow(v, n.path[i - 1].pos, n.path[i].pos, kPathColour);
    }
    const float* at = n.path.empty() ? entity : n.path.front().pos;
    if (!at || !NearCamera(v, at) || (n.path.empty() && n.events.empty())) return;
    char head[160];
    std::snprintf(head, sizeof(head), "%s: %s (%g to %gs)", s.name.c_str(), n.name.c_str(), s.start_time, s.end_time);
    std::string text = head;
    for (const std::string& e : n.events) text += "\n" + e;
    Label(v, at, text.c_str(), kPathColour);
}

}  // namespace

namespace fcip::editor {

std::vector<std::string> SequenceNames() {
    std::vector<std::string> names;
    for (const Sequence& s : *CurrentMovie()) names.push_back(s.name);
    std::sort(names.begin(), names.end());
    return names;
}

bool SequenceStart(const std::string& name, float out[3]) {
    for (const Sequence& s : *CurrentMovie()) {
        if (s.name != name || !s.has_start) continue;
        std::copy(s.start, s.start + 3, out);
        return true;
    }
    return false;
}

void DrawSequences(const View& v) {
    std::shared_ptr<const std::vector<ViewEntity>> entities = ViewEntities();
    std::unordered_map<std::string, const float*> places;
    for (const ViewEntity& e : *entities) places[e.name] = e.pos;
    for (const Sequence& s : *CurrentMovie()) {
        for (const MovieNode& n : s.nodes) {
            auto place = places.find(n.name);
            DrawNode(v, s, n, place == places.end() ? nullptr : place->second);
        }
    }
}

}  // namespace fcip::editor
