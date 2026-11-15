// the map editor's own tools, camera and undo as the desktop UI drives them (tools.cpp); window thread unless noted
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace fcip::editor {

enum Tool : int { kTerrain, kAddObject, kEditObject, kEnvironment, kToolCount };

// the terrain tool's actions: one of its four modes and the pad button the action is on
enum Brush : int { kRaise, kLower, kBump, kDig, kSetHeight, kExtend, kSmooth, kTrim, kPaint, kErase, kBrushCount };

constexpr int kRadiusSteps = 18;
constexpr float kMaxSetHeight = 225.0f;
constexpr int kTextureCount = 3;
constexpr int kCategoryCount = 8;
constexpr int kOptionCount = 9;
constexpr int kOptionOcean = 0;
constexpr int kOptionFog = 1;
constexpr int kOptionObjectIcons = 2;

// the environment tool's rows, kept in the map's settings
enum EnvironmentRow : int { kSound1, kSound2, kSound3, kFogDistance, kFogThickness, kTimeOfDay, kEnvironmentRows };
constexpr int kSoundCount = 20;
constexpr int kFogDistanceSteps = 10;
constexpr float kMinFogThickness = 0.2f;
constexpr float kMaxFogThickness = 1.0f;
constexpr int kTimesOfDay = 4;

// any thread: a map is open; ShellUp while editing, not in first person
bool MapOpen();
bool ShellUp();
bool ConsoleMenuUp();  // a console page (start menu, save, map info) has the editor's input
bool MapChanged();     // since it was last saved
std::string MapName(); // empty before it is first saved

// the tools as the editor has them now
struct State {
    int tool = kTerrain;
    int brush = kRaise;
    int radius_step = 0;
    float radius = 0.0f;  // metres
    float set_height = 0.0f;
    int texture = 0;  // what Paint lays: sand, grass, cliff
    bool square_cursor = false;
    float cursor[3] = {};  // where the tools act
    size_t undo_count = 0;
    bool applying = false;
    float environment[kEnvironmentRows] = {};
    bool has_environment = false;  // the map's settings are loaded
    bool options[kOptionCount] = {};
    int category = 0;  // the Add Object tool's
    int item = 0;
};
State Read();

const char* ToolName(int tool);
const char* BrushName(int brush);
const char* TextureName(int texture);
float RadiusMetres(int step);
const char* OptionName(int option);
const char* CategoryName(int category);
const char* TimeOfDayName(int time);
float FogDistanceMetres(int step);

void SelectTool(int tool);
void SelectBrush(int brush);
void SetRadiusStep(int step);
void SetSetHeight(float height);
void SetTexture(int texture);
void SetSquareCursor(bool square);
void Undo(int steps);
std::vector<std::string> History();  // the undo stack, oldest first
void SetEnvironment(int row, float value);
void SetOption(int option, bool on);
void DeleteSelected();  // the Edit Object tool's selection
void PickObject(int category, int item);  // into the Add Object tool, which a click in the view then places
// the start menu's buttons as a player presses them, and first person in the map and back
void Save();
void ExitToMapManager();  // asks to save first
void ShowControllerLayout();
void PlayTest();

// the Add Object tool's categories as last read on the editor's thread, with their object budgets
struct Category {
    int used = 0;
    int limit = 0;
    std::vector<std::string> items;
};
std::vector<Category> Catalogue();

// the mouse over the 3D view, each frame
struct ViewInput {
    bool inside = false;   // over the view, or dragging from it
    float u = 0.0f;        // its place in the frame, 0 to 1 from the top left
    float v = 0.0f;
    float look_x = 0.0f;   // pixels dragged with the right button
    float look_y = 0.0f;
    float wheel = 0.0f;    // notches, up is positive
    bool apply = false;    // the left button is held on the view
};
void SetViewInput(const ViewInput& in);

}  // namespace fcip::editor
