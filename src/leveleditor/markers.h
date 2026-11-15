// the level editor's view: what the game uses to run a level, drawn over the frame while editing (logic_markers.cpp)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

enum ViewCategory : int { kViewPlayerStart, kViewLogic, kViewAi, kViewCategoryCount };
const char* ViewCategoryName(int category);
inline bool g_view_shown[kViewCategoryCount] = {true, true, true};  // window thread, the View menu's toggles

// an entity as the last editing frame left it
struct ViewEntity {
    uint32_t id = 0;
    std::string cls;
    std::string name;
    float pos[3] = {};
    float angles[3] = {};   // degrees
    bool has_box = false;   // the world box the engine tests it with, as its triggers and physics see it
    float box_min[3] = {};
    float box_max[3] = {};
};
std::shared_ptr<const std::vector<ViewEntity>> ViewEntities();  // any thread

void UpdateMarkers(PPCContext& ctx);  // editor thread, each frame while editing
void DrawMarkers();                   // window thread, in the ImGui frame

// each category's drawing, window thread
void DrawPlayerStart(const View& v);  // player_markers.cpp

}  // namespace fcip::editor
