// the level editor's view: what the game uses to run a level, drawn over the frame while editing (logic_markers.cpp)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "leveleditor/mission.h"
#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

enum ViewCategory : int {
    kViewPlayerStart,
    kViewTriggers,
    kViewSequences,
    kViewCulling,
    kViewSounds,
    kViewLights,
    kViewGameplay,
    kViewAi,
    kViewCategoryCount
};
const char* ViewCategoryName(int category);
// window thread, the View menu's toggles
inline bool g_view_shown[kViewCategoryCount] = {true, true, true, true, true, true, true, true};

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
    std::vector<XmlAttribute> properties;  // Properties then PropertiesInstance, for the classes a category reads
};
std::shared_ptr<const std::vector<ViewEntity>> ViewEntities();  // any thread
std::string PropertyOf(const ViewEntity& e, const char* key);

// editor thread
void UpdateMarkers(PPCContext& ctx);  // each frame while editing
void ForgetProperties(uint32_t entity_id);  // its Properties changed
void ForgetMarkers();  // a level starts
void UpdateSectorGrid(PPCContext& ctx);  // culling_markers.cpp
void ForgetSectorGrid();

void DrawMarkers();  // window thread, in the ImGui frame

// each category's drawing, window thread
void DrawPlayerStart(const View& v);  // player_markers.cpp
void DrawTriggers(const View& v);     // trigger_markers.cpp
bool IsTrigger(const std::string& cls);  // any thread
void DrawSequences(const View& v);    // movie_markers.cpp
bool SequenceStart(const std::string& name, float out[3]);  // any thread, false for none such
void DrawCulling(const View& v);      // culling_markers.cpp
void DrawAi(const View& v);           // ai_markers.cpp
bool IsAiEntity(const std::string& cls);  // any thread
void DrawSounds(const View& v);       // sound_markers.cpp
void DrawLights(const View& v);
bool IsSoundOrLight(const std::string& cls);  // any thread
void DrawGameplay(const View& v);     // gameplay_markers.cpp
bool IsGameplay(const std::string& cls);  // any thread
void DrawArea(const View& v, const AiObject& area, ImU32 colour);  // a Shape, AreaBox or AreaSphere (area_markers.cpp)
// the areas naming the entity, each with a line to it
void DrawEntityAreas(const View& v, const std::string& name, const float pos[3], ImU32 colour);

}  // namespace fcip::editor
