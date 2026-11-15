// ige.xex, the solo level editor: where the player starts and the checkpoints. The game spawns the player at the
// first respawn point (the solo GameRules' Server:GetFirstRespawnPoint), which the mission's first Respawn object
// makes as its Objects load (default.xex 82136CB0), facing its Angles. A Checkpoint saves the whole game a moment
// after the player enters the box the engine tests it with, so after a death the player comes back where he stood
// as it saved; before the first checkpoint, at the player start again.
#include <cstdio>
#include <memory>
#include <string>

#include <imgui.h>

#include "leveleditor/markers.h"
#include "leveleditor/mission.h"

using namespace fcip::editor;

namespace {

constexpr ImU32 kStartColour = IM_COL32(90, 220, 110, 255);
constexpr ImU32 kRespawnColour = IM_COL32(90, 220, 110, 150);
constexpr ImU32 kCheckpointColour = IM_COL32(80, 190, 240, 255);
constexpr float kFacingMetres = 3.0f;

bool ReadAngles(const AiObject& o, float out[3]) {
    std::string text = AttributeOf(o, "Angles");
    return std::sscanf(text.c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

void DrawRespawn(const View& v, const AiObject& o, const char* label, ImU32 colour) {
    Square(v, o.pos, colour);
    Label(v, o.pos, label, colour);
    float angles[3] = {};
    if (!ReadAngles(o, angles)) return;
    float axes[3][3];
    AxesFromAngles(angles, axes);
    const float* facing = axes[1];
    float ahead[3];
    for (int i = 0; i < 3; i++) ahead[i] = o.pos[i] + facing[i] * kFacingMetres;
    Arrow(v, o.pos, ahead, colour);
}

void DrawRespawns(const View& v) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    int count = 0;
    for (const AiObject& o : ai->objects) {
        if (o.type != "Respawn" || !o.placed) continue;
        // an unnamed one is named by its place in the list
        std::string name = o.name.empty() ? "Respawn" + std::to_string(count) : o.name;
        if (count == 0) DrawRespawn(v, o, ("PLAYER START (" + name + ")").c_str(), kStartColour);
        else DrawRespawn(v, o, name.c_str(), kRespawnColour);
        count++;
    }
}

void DrawCheckpoints(const View& v) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.cls != "Checkpoint") continue;
        std::string label = "Checkpoint " + e.name;
        if (!e.has_box) {
            Square(v, e.pos, kCheckpointColour);
            Label(v, e.pos, label.c_str(), kCheckpointColour);
            continue;
        }
        AxisBox(v, e.box_min, e.box_max, kCheckpointColour);
        float top[3] = {(e.box_min[0] + e.box_max[0]) * 0.5f, (e.box_min[1] + e.box_max[1]) * 0.5f, e.box_max[2]};
        Label(v, top, label.c_str(), kCheckpointColour);
    }
}

}  // namespace

void fcip::editor::DrawPlayerStart(const View& v) {
    DrawCheckpoints(v);
    DrawRespawns(v);
}
