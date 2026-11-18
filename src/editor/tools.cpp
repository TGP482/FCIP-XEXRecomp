// ige.xex: the map editor's tools, camera and undo driven from the desktop UI. Commands queued by the UI run on the
// editor's thread. The mouse turns and zooms the camera through the editor's own look inputs, moves the tool cursor
// to where it points, and holds the pad button the active tool acts on.
#include "editor/tools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>

#include "common/input.h"
#include "common/keys.h"
#include "common/view_clip.h"
#include "core/input.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// EditorInterfaceHandler, a static object: +0x104 1 editing or 2 first person, +0x108 the tool, +0x10C the tool
// object; the four tools live inside it
constexpr uint32_t kEditorHandler = 0x8296F758;
constexpr uint32_t kHandlerMode = 0x104;
constexpr uint32_t kHandlerTool = 0x108;
constexpr uint32_t kHandlerToolObject = 0x10C;
constexpr uint32_t kModeEditing = 1;
// terrain tool: +0x108 mode (bump, raise, set height, paint), +0x10C radius step, +0x12C paint texture
constexpr uint32_t kTerrainTool = kEditorHandler + 0x110;
constexpr uint32_t kTerrainMode = 0x108;
constexpr uint32_t kTerrainRadiusStep = 0x10C;
constexpr uint32_t kTerrainTexture = 0x12C;
constexpr uint32_t kRadiusTable = 0x82067C10;  // metres per step
constexpr uint32_t kRadius = 0x8296B1BC;       // the step's metres, which the brushes read
constexpr uint32_t kHeightTarget = 0x828C4A54;
constexpr uint32_t kSquareCursor = 0x8296B1D4;
constexpr uint32_t kToolCursor = 0x828C5D50;   // xyz, where the tools act
constexpr uint32_t kMapChanged = 0x8296B266;
// camera: distance 1 to 115 from what it orbits; mode 2 casts its view's centre ray, the others orbit the cursor
constexpr uint32_t kCameraDistance = 0x828C49A4;
constexpr uint32_t kCameraMode = 0x8296B240;
constexpr uint32_t kCameraRaycast = 2;
constexpr uint32_t kFrameSeconds = 0x828C7310;
// undo: a vector of command objects with methods destroy(this, 1) and undo(this)
constexpr uint32_t kUndoFirst = 0x828C632C;
constexpr uint32_t kUndoLast = 0x828C6330;
constexpr uint32_t kUndoDestroy = 0x0;
constexpr uint32_t kUndoRun = 0x4;
constexpr uint32_t kToolUndone = 0x3C;  // tool method
constexpr size_t kUndoKept = 5;
constexpr uint32_t kEngineTerrain = 0x364;
// the terrain's surface types in LevelData order (CTerrain::SetDetailTextures 82469CE0)
constexpr uint32_t kSurfaceBytes = 0x1BC;
constexpr uint32_t kSurfaceTexture = 0xB0;  // Texture_None when unused
constexpr uint32_t kSurfaceName = 0x1EC;
// add object tool: +0x244 the category object shown, +0x248 the item picked in each category, +0x26C its category
// objects, +0x27C the category shown
constexpr uint32_t kAddTool = kEditorHandler + 0x248;
constexpr uint32_t kAddShown = 0x244;
constexpr uint32_t kAddPicked = 0x248;
constexpr uint32_t kAddCategories = 0x26C;
constexpr uint32_t kAddCategory = 0x27C;
// category methods: count(this), item name(out wstring, this, index), objects placed(this), their limit(this); the
// last two look up state the category gets once shown (respawn points crash before)
constexpr uint32_t kCategoryItems = 0x0;
constexpr uint32_t kCategoryItemName = 0x4;
constexpr uint32_t kCategoryPlaced = 0x10;
constexpr uint32_t kCategoryLimit = 0x14;
constexpr int kCatalogueFrames = 30;  // between reads of the catalogue
// environment tool: +0x110 changes since its row was picked; the editor's manager holds its values
constexpr uint32_t kEnvironmentTool = kEditorHandler + 0x628;
constexpr uint32_t kEnvironmentChanges = 0x110;
// the editor's interface manager, which also holds the map's settings and name (+0x68, 16 UTF-16 characters)
constexpr uint32_t kManager = 0x8296B2CC;
constexpr uint32_t kMapName = 0x68;
constexpr uint32_t kMapNameChars = 16;
constexpr uint32_t kHandlerActive = 0xF0;
// the start menu, a static object, and its buttons by their names' crc32
constexpr uint32_t kStartMenuListener = 0x82971908 + 0x8;
constexpr uint32_t kStartMenuName = 0x82067EBC;  // "StartMenuInterfaceHandler"
constexpr uint32_t kButtonSave = 0x6535D687u;     // B_SaveMap
constexpr uint32_t kButtonExit = 0xD23F714Cu;     // B_ExitEditor
constexpr uint32_t kButtonControls = 0xB297004Cu; // B_Controls
constexpr uint32_t kMapTimeOfDay = 0x1F8;
constexpr uint32_t kMapFogThickness = 0x1FC;
constexpr uint32_t kMapFogDistance = 0x200;
constexpr uint32_t kMapSounds = 0x220;
constexpr uint32_t kFogDistanceTable = 0x828C49D8;
// editing options, bytes in the object at *0x8296B130, in the options list's order
constexpr uint32_t kOptions = 0x8296B130;
constexpr uint32_t kOptionBytes[] = {0x414, 0x417, 0x410, 0x411, 0x412, 0x413, 0x415, 0x416, 0x418};
constexpr uint32_t kConsole = 0x828C71DC;
constexpr uint32_t kOceanVariable = 0x82068614;  // "e_water_ocean"
// a page draws its widgets: r3 the page, +0x14 its name's crc32
constexpr uint32_t kPageName = 0x14;
constexpr uint32_t kConsolePages[] = {
    0xE2067571u,  // P_MainEditor
    0x436A5C60u,  // P_TerrainToolBar
    0xC0BC392Du,  // P_ObjectToolAdd
    0xFAB953FEu,  // P_ObjectToolEdit
    0xCBD116F2u,  // P_EnvironmentTool
    0x166632B8u,  // P_RTriggerMenuButtons
};
constexpr uint32_t kMapManagerPage = 0x8811FEBDu;  // P_MapManager
// editor input actions the camera reads
constexpr uint32_t kActionLookX = 0x12;
constexpr uint32_t kActionLookY = 0x13;
constexpr uint32_t kActionPitchHeld = 0xC;

// guest functions
constexpr uint32_t kSelectTool = 0x820C5340;  // (handler, tool, 1)
constexpr uint32_t kTerrainModeButtons = 0x820D20B8;
constexpr uint32_t kTerrainModeCaptions = 0x820D0C78;
constexpr uint32_t kRadiusCaption = 0x820D1BE8;
constexpr uint32_t kHeightCaption = 0x820D19F0;
constexpr uint32_t kTextureCaption = 0x820D18D0;
constexpr uint32_t kUndoPop = 0x8210DDD8;
constexpr uint32_t kTerrainHeight = 0x82324AA8;  // (x f1, y f2, terrain)
constexpr uint32_t kClampToMap = 0x82103000;    // (out, in), returns out
constexpr uint32_t kCategoryShown = 0x820D6498;  // (add tool) after its category changes
constexpr uint32_t kItemShown = 0x820D6788;      // (add tool) after its item changes
constexpr uint32_t kEnvironmentUndo = 0x820DFE50;
constexpr uint32_t kSoundApplied[] = {0x820E0A70, 0x820E0AE8, 0x820E0B60};
constexpr uint32_t kTimeOfDayApplied = 0x820E0240;
constexpr uint32_t kTimeOfDayLight = 0x820E0880;
constexpr uint32_t kEnvironmentCaptions = 0x820E0940;
constexpr uint32_t kSetFogDistance = 0x823364A0;   // (f1, engine)
constexpr uint32_t kSetFogThickness = 0x823364A8;  // (f1, engine)
constexpr uint32_t kFindVariable = 0x8233DF48;     // (console, name)
constexpr uint32_t kSetVariable = 0x8233C0B0;      // (variable, value)
constexpr uint32_t kFree = 0x8230F2D8;
constexpr uint32_t kShowHandler = 0x82113178;     // (manager, handler name)
constexpr uint32_t kStartMenuPress = 0x820C2C88;  // (listener, widget), the widget's name crc at +4
constexpr uint32_t kSetEditorMode = 0x820C5548;   // (handler, 1 editing or 2 first person)

// how the editor turns its look inputs into motion each second, an input of v moving v * |v| times this
constexpr double kYawPerSecond = 90.0;
constexpr double kPitchPerSecond = 60.0;
constexpr double kLookDegreesPerPixel = 0.2;
constexpr double kZoomPerNotch = 0.15;  // of the distance
// the pick ray against the terrain
constexpr float kPickReach = 500.0f;
constexpr float kPickStep = 2.0f;
constexpr int kPickRefine = 8;

struct BrushInfo {
    const char* name;
    int mode;  // -1 works in any mode
    uint16_t button;
};
constexpr BrushInfo kBrushes[] = {
    {"Raise", 1, input::kY},      {"Lower", 1, input::kA},  {"Bump", 0, input::kY},   {"Dig", 0, input::kA},
    {"Set Height", 2, input::kA}, {"Extend", 2, input::kY}, {"Smooth", -1, input::kB}, {"Trim", -1, input::kX},
    {"Paint", 3, input::kA},      {"Erase", 3, input::kY},
};
static_assert(std::size(kBrushes) == fcip::editor::kBrushCount);
constexpr const char* kToolNames[] = {"Terrain", "Add Object", "Edit Object", "Environment"};
constexpr const char* kModeNames[] = {"Bump", "Raise", "Set Height", "Paint"};
constexpr const char* kTextureNames[] = {"Sand", "Grass", "Cliff"};
constexpr const char* kTimeNames[] = {"Dawn", "Day", "Dusk", "Night"};
constexpr const char* kCategoryNames[] = {"Vehicles",   "Weapons and Items", "Multiplayer",    "Respawn Points",
                                          "Special",    "Small Brushes",     "Large Brushes",  "Trees"};
// as the solo catalogue fills them (campaign_catalogue.cpp)
constexpr const char* kSoloCategoryNames[] = {"Vehicles and Objects", "Weapons and Pickups", "Enemies",
                                              "Respawn Points",       "Special",             "Small Brushes",
                                              "Large Brushes",        "Trees"};
static_assert(std::size(kSoloCategoryNames) == std::size(kCategoryNames));
constexpr const char* kOptionNames[] = {
    "Draw Ocean Water", "Show Fog", "Draw Object Icons", "Snap Brushes to Terrain", "Snap Fences and Bridges",
    "Prompt Before Deleting", "Keep Large Brushes and Ladders off Terrain Edits", "Snap Ladders to Sniper Towers",
    "Tree Collisions",
};
static_assert(std::size(kOptionNames) == std::size(kOptionBytes));
constexpr auto kPulse = std::chrono::milliseconds(120);  // a pad press long enough for the editor to see it

enum class Op {
    SelectTool, TerrainMode, RadiusStep, SetHeight, Texture, SquareCursor, Undo, Environment, Option, Pick,
    StartMenu, PlayTest,
};
struct Command {
    Op op;
    int value = 0;
    float amount = 0.0f;
    int item = 0;
};

struct Vec {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool operator==(const Vec&) const = default;
};

std::atomic<bool> g_map_open{false};  // from the main editor showing until the map manager does
std::atomic<int> g_brush{fcip::editor::kRaise};
std::atomic<bool> g_apply{false};
std::atomic<uint16_t> g_pulse{0};  // a button pressed once
std::atomic<int64_t> g_pulse_until{0};

std::mutex g_lock;
std::vector<Command> g_commands;
std::deque<fcip::editor::UndoStep> g_history;  // labels beside the editor's undo stack
std::atomic<uint64_t> g_stamp{0};
fcip::editor::ViewInput g_view;      // the latest, deltas summed until the camera takes them
std::vector<fcip::editor::Category> g_catalogue;

// editor thread
struct Inject {
    bool on = false;
    double look_x = 0.0;
    double look_y = 0.0;
    bool pitch = false;
    bool ray = false;
    float dir[3] = {};
};
thread_local Inject t_inject;
bool g_have_orbit = false;
Vec g_orbit;    // what the camera orbits, kept while the cursor follows the mouse
Vec g_written;  // the cursor as we left it
uint32_t g_scratch = 0;
uint32_t g_name_scratch = 0;
uint32_t g_widget_scratch = 0;
int g_catalogue_wait = 0;
bool g_category_shown[fcip::editor::kCategoryCount] = {};  // since the map opened
int g_environment_row = -1;  // the row the last change was to

void Queue(Command c) {
    std::lock_guard lock(g_lock);
    g_commands.push_back(c);
}

void WF(uint32_t at, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    W32(at, bits);
}

Vec ReadVec(uint32_t at) { return {GF(at), GF(at + 4), GF(at + 8)}; }
void WriteVec(uint32_t at, const Vec& v) {
    WF(at, v.x);
    WF(at + 4, v.y);
    WF(at + 8, v.z);
}

uint32_t Method(uint32_t object, uint32_t slot) { return R32(R32(object) + slot); }

size_t UndoCount() {
    uint32_t first = R32(kUndoFirst);
    return first ? (R32(kUndoLast) - first) / 4 : 0;
}

int CurrentTool() { return int(R32(kEditorHandler + kHandlerTool)); }

// the chosen brush when it works in the terrain mode set, else that mode's first
int BrushFor(int mode) {
    int brush = g_brush;
    if (kBrushes[brush].mode < 0 || kBrushes[brush].mode == mode) return brush;
    for (int b = 0; b < fcip::editor::kBrushCount; b++) {
        if (kBrushes[b].mode == mode) return b;
    }
    return brush;
}

// Commands, editor thread

std::vector<fcip::editor::UndoStep> ExtraSteps() {
    auto& extra = fcip::editor::g_extra_undo;
    return extra.steps ? extra.steps() : std::vector<fcip::editor::UndoStep>();
}

// the editor's newest step's stamp, 0 for one made before its label was kept
uint64_t NativeStamp() {
    std::lock_guard lock(g_lock);
    if (UndoCount() == 0 || g_history.size() < UndoCount()) return 0;
    return g_history.back().stamp;
}

void RunUndo(PPCContext& ctx) {
    std::vector<fcip::editor::UndoStep> extra = ExtraSteps();
    if (!extra.empty() && (UndoCount() == 0 || extra.back().stamp > NativeStamp())) {
        fcip::editor::g_extra_undo.undo(ctx);
        return;
    }
    uint32_t command = uint32_t(CallGuest(ctx, kUndoPop, {}));
    if (!command) return;
    CallGuest(ctx, Method(command, kUndoRun), {command});
    CallGuest(ctx, Method(command, kUndoDestroy), {command, 1});
    W8(kMapChanged, 1);
    uint32_t tool = R32(kEditorHandler + kHandlerToolObject);
    if (tool) CallGuest(ctx, Method(tool, kToolUndone), {tool});
}

// the terrain tool's captions exist while its page is up
void Caption(PPCContext& ctx, uint32_t fn) {
    if (CurrentTool() == fcip::editor::kTerrain) CallGuest(ctx, fn, {kTerrainTool});
}

// a float argument in f1 for one call
void CallWithFloat(PPCContext& ctx, uint32_t fn, double value, uint32_t arg) {
    double f1 = ctx.f[1];
    ctx.f[1] = value;
    CallGuest(ctx, fn, {arg});
    ctx.f[1] = f1;
}

// each row's first change since another row's pushes an undo, as the tool does
void RunEnvironment(PPCContext& ctx, int row, float value) {
    uint32_t settings = R32(kManager);
    if (!settings || CurrentTool() != fcip::editor::kEnvironment) return;
    uint32_t changes = kEnvironmentTool + kEnvironmentChanges;
    if (row != g_environment_row) W32(changes, 0);
    g_environment_row = row;
    if (R32(changes) == 0) CallGuest(ctx, kEnvironmentUndo, {kEnvironmentTool});
    W32(changes, R32(changes) + 1);
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    int step = int(std::lround(value));
    switch (row) {
        case fcip::editor::kSound1:
        case fcip::editor::kSound2:
        case fcip::editor::kSound3:
            W32(settings + kMapSounds + uint32_t(row) * 4, uint32_t(step));
            CallGuest(ctx, kSoundApplied[row], {kEnvironmentTool});
            break;
        case fcip::editor::kFogDistance:
            W32(settings + kMapFogDistance, uint32_t(step));
            CallWithFloat(ctx, kSetFogDistance, GF(kFogDistanceTable + uint32_t(step) * 4), engine);
            break;
        case fcip::editor::kFogThickness: {
            WF(settings + kMapFogThickness, value);
            bool fog = R8(R32(kOptions) + kOptionBytes[fcip::editor::kOptionFog]) != 0;
            CallWithFloat(ctx, kSetFogThickness, fog ? value : 0.0, engine);
            break;
        }
        case fcip::editor::kTimeOfDay:
            W32(settings + kMapTimeOfDay, uint32_t(step));
            CallGuest(ctx, kTimeOfDayApplied, {kEnvironmentTool});
            CallGuest(ctx, kTimeOfDayLight, {kEnvironmentTool});
            break;
    }
    W8(kMapChanged, 1);
    CallGuest(ctx, kEnvironmentCaptions, {kEnvironmentTool});
}

// as the options list sets them, the ocean and fog applied at once
void RunOption(PPCContext& ctx, int option, bool on) {
    uint32_t options = R32(kOptions);
    if (!options) return;
    W8(options + kOptionBytes[option], on ? 1 : 0);
    if (option == fcip::editor::kOptionOcean) {
        uint32_t variable = uint32_t(CallGuest(ctx, kFindVariable, {R32(kConsole), kOceanVariable}));
        if (variable) CallGuest(ctx, kSetVariable, {variable, on ? 1u : 0u});
    }
    uint32_t settings = R32(kManager);
    if (option == fcip::editor::kOptionFog && settings) {
        CallWithFloat(ctx, kSetFogThickness, on ? GF(settings + kMapFogThickness) : 0.0, R32(SYM_C3DEngine_pointer));
    }
}

void RunPick(PPCContext& ctx, int category, int item) {
    if (CurrentTool() != fcip::editor::kAddObject) return;
    W32(kAddTool + kAddCategory, uint32_t(category));
    CallGuest(ctx, kCategoryShown, {kAddTool});
    W32(kAddTool + kAddPicked + uint32_t(category) * 4, uint32_t(item));
    CallGuest(ctx, kItemShown, {kAddTool});
}

// a start menu button pressed as a player does: the menu shown, then its button
void RunStartMenu(PPCContext& ctx, uint32_t button) {
    constexpr uint32_t kWidgetName = 0x4;
    if (!g_widget_scratch) g_widget_scratch = mem::AllocSystem(8);
    if (!g_widget_scratch) return;
    CallGuest(ctx, kShowHandler, {R32(kManager), kStartMenuName});
    W32(g_widget_scratch + kWidgetName, button);
    CallGuest(ctx, kStartMenuPress, {kStartMenuListener, g_widget_scratch});
}

void RunPlayTest(PPCContext& ctx) {
    uint32_t mode = R32(kEditorHandler + kHandlerMode) == kModeEditing ? 2 : kModeEditing;
    CallGuest(ctx, kSetEditorMode, {kEditorHandler, mode});
}

void Run(PPCContext& ctx, const Command& c) {
    switch (c.op) {
        case Op::SelectTool:
            if (CurrentTool() != c.value) CallGuest(ctx, kSelectTool, {kEditorHandler, uint32_t(c.value), 1});
            break;
        case Op::TerrainMode:
            W32(kTerrainTool + kTerrainMode, uint32_t(c.value));
            Caption(ctx, kTerrainModeButtons);
            Caption(ctx, kTerrainModeCaptions);
            break;
        case Op::RadiusStep:
            W32(kTerrainTool + kTerrainRadiusStep, uint32_t(c.value));
            WF(kRadius, GF(kRadiusTable + uint32_t(c.value) * 4));
            Caption(ctx, kRadiusCaption);
            break;
        case Op::SetHeight:
            WF(kHeightTarget, c.amount);
            Caption(ctx, kHeightCaption);
            break;
        case Op::Texture:
            W32(kTerrainTool + kTerrainTexture, uint32_t(c.value));
            Caption(ctx, kTextureCaption);
            break;
        case Op::SquareCursor: W32(kSquareCursor, uint32_t(c.value)); break;
        case Op::Undo: RunUndo(ctx); break;
        case Op::Environment: RunEnvironment(ctx, c.value, c.amount); break;
        case Op::Option: RunOption(ctx, c.value, c.item != 0); break;
        case Op::Pick: RunPick(ctx, c.value, c.item); break;
        case Op::StartMenu: RunStartMenu(ctx, uint32_t(c.value)); break;
        case Op::PlayTest: RunPlayTest(ctx); break;
    }
}

// a std::wstring the guest made, its UTF-16 as UTF-8; freed after
std::string TakeWideString(PPCContext& ctx, uint32_t str) {
    constexpr uint32_t kText = 0x04;
    constexpr uint32_t kSize = 0x14;
    constexpr uint32_t kCapacity = 0x18;
    constexpr uint32_t kInlineChars = 8;
    bool heap = R32(str + kCapacity) >= kInlineChars;
    uint32_t text = heap ? R32(str + kText) : str + kText;
    uint32_t size = std::min(R32(str + kSize), 256u);
    std::string out;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t c = R16(text + i * 2);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    if (heap) CallGuest(ctx, kFree, {text});
    return out;
}

// the categories' budgets each read, their items again only when their count changes
void ReadCatalogue(PPCContext& ctx) {
    constexpr uint32_t kWideStringBytes = 0x1C;
    if (!g_name_scratch) g_name_scratch = mem::AllocSystem(kWideStringBytes);
    uint32_t first = R32(kAddTool + kAddCategories);
    if (!first || !g_name_scratch) return;
    std::vector<fcip::editor::Category> catalogue;
    {
        std::lock_guard lock(g_lock);
        catalogue = g_catalogue;
    }
    catalogue.resize(fcip::editor::kCategoryCount);
    for (uint32_t c = 0; c < uint32_t(fcip::editor::kCategoryCount); c++) {
        uint32_t category = R32(first + c * 4);
        if (!category) continue;
        fcip::editor::Category& k = catalogue[c];
        if (category == R32(kAddTool + kAddShown)) g_category_shown[c] = true;
        if (g_category_shown[c]) {
            k.used = int(CallGuest(ctx, Method(category, kCategoryPlaced), {category}));
            k.limit = int(CallGuest(ctx, Method(category, kCategoryLimit), {category})) * fcip::editor::BudgetScale();
        }
        uint32_t count = uint32_t(CallGuest(ctx, Method(category, kCategoryItems), {category}));
        if (count == k.items.size()) continue;
        k.items.clear();
        for (uint32_t i = 0; i < count; i++) {
            CallGuest(ctx, Method(category, kCategoryItemName), {g_name_scratch, category, i});
            k.items.push_back(TakeWideString(ctx, g_name_scratch));
        }
    }
    std::lock_guard lock(g_lock);
    g_catalogue = std::move(catalogue);
}

void RunCommands(PPCContext& ctx) {
    std::vector<Command> commands;
    {
        std::lock_guard lock(g_lock);
        commands.swap(g_commands);
    }
    for (const Command& c : commands) Run(ctx, c);
    if (--g_catalogue_wait > 0) return;
    g_catalogue_wait = kCatalogueFrames;
    ReadCatalogue(ctx);
}

// Camera and pick, editor thread

// the input v with v * |v| * rate == change
double InputFor(double change, double rate) {
    if (rate <= 0.0 || change == 0.0) return 0.0;
    double q = change / rate;
    return std::copysign(std::sqrt(std::fabs(q)), q);
}

// the editor's zoom speed at a distance
double ZoomRate(double distance) {
    if (distance < 10.0) return 15.0;
    if (distance < 25.0) return 30.0;
    return 50.0;
}

// the look this frame from the mouse; a pitch and a zoom share an input, so a zoom waits for a frame without pitch
Inject TakeLook() {
    Inject in;
    double dt = GF(kFrameSeconds);
    std::lock_guard lock(g_lock);
    if (dt <= 0.0) return in;
    in.look_x = InputFor(g_view.look_x * kLookDegreesPerPixel, dt * kYawPerSecond);
    if (g_view.look_y != 0.0f) {
        in.pitch = true;
        in.look_y = InputFor(g_view.look_y * kLookDegreesPerPixel, dt * kPitchPerSecond);
    } else if (g_view.wheel != 0.0f) {
        double distance = GF(kCameraDistance);
        in.look_y = InputFor(g_view.wheel * distance * kZoomPerNotch, dt * ZoomRate(distance));
        g_view.wheel = 0.0f;
    }
    g_view.look_x = 0.0f;
    g_view.look_y = 0.0f;
    in.on = true;
    return in;
}

bool Picking() { return g_view.inside && !fcip::keys::PadRecent(); }

double TerrainHeight(PPCContext& ctx, uint32_t terrain, float x, float y) {
    double f1 = ctx.f[1];
    double f2 = ctx.f[2];
    ctx.f[1] = x;
    ctx.f[2] = y;
    CallGuest(ctx, kTerrainHeight, {terrain});
    double h = ctx.f[1];
    ctx.f[1] = f1;
    ctx.f[2] = f2;
    return h;
}

// where the ray first goes under the terrain, marched then halved down
bool TerrainHit(PPCContext& ctx, const float o[3], const float d[3], Vec& hit) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t terrain = engine ? R32(engine + kEngineTerrain) : 0;
    if (!terrain) return false;
    auto above = [&](float t) {
        return o[2] + d[2] * t > TerrainHeight(ctx, terrain, o[0] + d[0] * t, o[1] + d[1] * t);
    };
    float before = 0.0f;
    float under = kPickStep;
    while (above(under)) {
        before = under;
        under += kPickStep;
        if (under > kPickReach) return false;
    }
    for (int i = 0; i < kPickRefine; i++) {
        float mid = (before + under) * 0.5f;
        if (above(mid)) before = mid;
        else under = mid;
    }
    hit = {o[0] + d[0] * under, o[1] + d[1] * under, 0.0f};
    hit.z = float(TerrainHeight(ctx, terrain, hit.x, hit.y));
    return true;
}

Vec ClampToMap(PPCContext& ctx, const Vec& v) {
    constexpr uint32_t kVecBytes = 12;
    if (!g_scratch) g_scratch = mem::AllocSystem(kVecBytes * 2);
    if (!g_scratch) return v;
    WriteVec(g_scratch, v);
    uint32_t out = uint32_t(CallGuest(ctx, kClampToMap, {g_scratch + kVecBytes, g_scratch}));
    return out ? ReadVec(out) : v;
}

bool MouseRay(float origin[3], float dir[3]) {
    float u, v;
    {
        std::lock_guard lock(g_lock);
        if (!Picking()) return false;
        u = g_view.u;
        v = g_view.v;
    }
    if (!fcip::ViewRay(u, v, origin, dir)) return false;
    float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length <= 0.0f) return false;
    for (int i = 0; i < 3; i++) dir[i] /= length;
    return true;
}

// the orbit cursor back, unless the editor moved the cursor itself since
void RestoreOrbit() {
    if (g_have_orbit && ReadVec(kToolCursor) == g_written) WriteVec(kToolCursor, g_orbit);
}

void PlaceCursor(PPCContext& ctx) {
    g_orbit = ReadVec(kToolCursor);
    g_have_orbit = true;
    float origin[3];
    float dir[3];
    Vec hit;
    if (MouseRay(origin, dir) && TerrainHit(ctx, origin, dir, hit)) WriteVec(kToolCursor, ClampToMap(ctx, hit));
    g_written = ReadVec(kToolCursor);
}

}  // namespace

namespace fcip::editor {

bool MapOpen() { return g_map_open; }
bool ShellUp() { return g_map_open && R32(kEditorHandler + kHandlerMode) == kModeEditing; }
bool ConsoleMenuUp() { return R8(kEditorHandler + kHandlerActive) == 0; }
bool MapChanged() { return R8(kMapChanged) != 0; }

std::string MapName() {
    uint32_t manager = R32(kManager);
    std::string name;
    for (uint32_t i = 0; manager && i < kMapNameChars; i++) {
        uint16_t c = R16(manager + kMapName + i * 2);
        if (!c) break;
        name += c < 0x80 ? char(c) : '?';
    }
    return name;
}

void Save() { Queue({Op::StartMenu, int(kButtonSave)}); }
void ExitToMapManager() { Queue({Op::StartMenu, int(kButtonExit)}); }
void ShowControllerLayout() { Queue({Op::StartMenu, int(kButtonControls)}); }
void PlayTest() { Queue({Op::PlayTest}); }

State Read() {
    State s;
    s.tool = std::clamp(CurrentTool(), 0, kToolCount - 1);
    int mode = int(R32(kTerrainTool + kTerrainMode));
    s.brush = BrushFor(mode);
    s.radius_step = std::clamp(int(R32(kTerrainTool + kTerrainRadiusStep)), 0, kRadiusSteps - 1);
    s.radius = GF(kRadius);
    s.set_height = GF(kHeightTarget);
    s.texture = std::clamp(int(R32(kTerrainTool + kTerrainTexture)), 0, kMaxTextures - 1);
    s.square_cursor = R32(kSquareCursor) != 0;
    Vec c = ReadVec(kToolCursor);
    s.cursor[0] = c.x;
    s.cursor[1] = c.y;
    s.cursor[2] = c.z;
    s.undo_count = UndoCount() + ExtraSteps().size();
    s.applying = g_apply;
    if (uint32_t settings = R32(kManager)) {
        s.has_environment = true;
        for (int i = kSound1; i <= kSound3; i++) s.environment[i] = float(R32(settings + kMapSounds + uint32_t(i) * 4));
        s.environment[kFogDistance] = float(R32(settings + kMapFogDistance));
        s.environment[kFogThickness] = GF(settings + kMapFogThickness);
        s.environment[kTimeOfDay] = float(R32(settings + kMapTimeOfDay));
    }
    if (uint32_t options = R32(kOptions)) {
        for (int i = 0; i < kOptionCount; i++) s.options[i] = R8(options + kOptionBytes[i]) != 0;
    }
    s.category = std::clamp(int(R32(kAddTool + kAddCategory)), 0, kCategoryCount - 1);
    s.item = int(R32(kAddTool + kAddPicked + uint32_t(s.category) * 4));
    return s;
}

float TerrainHeightAt(PPCContext& ctx, float x, float y) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t terrain = engine ? R32(engine + kEngineTerrain) : 0;
    return terrain ? float(TerrainHeight(ctx, terrain, x, y)) : 0.0f;
}

const char* ToolName(int tool) { return kToolNames[std::clamp(tool, 0, kToolCount - 1)]; }
const char* BrushName(int brush) { return kBrushes[std::clamp(brush, 0, kBrushCount - 1)].name; }

// the map editor's three, or a campaign level's own
std::vector<std::string> PaintTextures() {
    if (!g_solo || !CampaignBase()) return {std::begin(kTextureNames), std::end(kTextureNames)};
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t terrain = engine ? R32(engine + kEngineTerrain) : 0;
    std::vector<std::string> names;
    for (uint32_t i = 0; terrain && i < uint32_t(kMaxTextures); i++) {
        uint32_t type = terrain + i * kSurfaceBytes;
        if (R32(type + kSurfaceTexture) == R32(SYM_Texture_None)) continue;
        names.resize(i + 1);
        names[i] = ReadCString(type + kSurfaceName);
    }
    return names;
}

float RadiusMetres(int step) { return GF(kRadiusTable + uint32_t(std::clamp(step, 0, kRadiusSteps - 1)) * 4); }
const char* OptionName(int option) { return kOptionNames[std::clamp(option, 0, kOptionCount - 1)]; }
const char* CategoryName(int category) {
    const char* const* names = g_solo ? kSoloCategoryNames : kCategoryNames;
    return names[std::clamp(category, 0, kCategoryCount - 1)];
}
const char* TimeOfDayName(int time) { return kTimeNames[std::clamp(time, 0, kTimesOfDay - 1)]; }
float FogDistanceMetres(int step) {
    return GF(kFogDistanceTable + uint32_t(std::clamp(step, 0, kFogDistanceSteps - 1)) * 4);
}

void SelectTool(int tool) { Queue({Op::SelectTool, std::clamp(tool, 0, kToolCount - 1)}); }

void SelectBrush(int brush) {
    brush = std::clamp(brush, 0, kBrushCount - 1);
    g_brush = brush;
    Queue({Op::SelectTool, kTerrain});
    if (kBrushes[brush].mode >= 0) Queue({Op::TerrainMode, kBrushes[brush].mode});
}

void SetRadiusStep(int step) { Queue({Op::RadiusStep, std::clamp(step, 0, kRadiusSteps - 1)}); }

float MaxSetHeight() { return fcip::editor::g_solo ? kHeightmapMaxHeight : kMapMaxHeight; }
void SetSetHeight(float height) { Queue({Op::SetHeight, 0, std::clamp(height, 0.0f, MaxSetHeight())}); }
void SetTexture(int texture) {
    int last = std::max(int(PaintTextures().size()) - 1, 0);
    Queue({Op::Texture, std::clamp(texture, 0, last)});
}
void SetSquareCursor(bool square) { Queue({Op::SquareCursor, square ? 1 : 0}); }

void SetEnvironment(int row, float value) { Queue({Op::Environment, std::clamp(row, 0, kEnvironmentRows - 1), value}); }
void SetOption(int option, bool on) { Queue({Op::Option, std::clamp(option, 0, kOptionCount - 1), 0.0f, on ? 1 : 0}); }

// the Edit Object tool deletes on the left shoulder
void DeleteSelected() {
    if (CurrentTool() != kEditObject) return;
    auto until = std::chrono::steady_clock::now() + kPulse;
    g_pulse_until = until.time_since_epoch().count();
    g_pulse = input::kLeftShoulder;
}

void PickObject(int category, int item) {
    Queue({Op::SelectTool, kAddObject});
    Queue({Op::Pick, std::clamp(category, 0, kCategoryCount - 1), 0.0f, std::max(item, 0)});
}

std::vector<Category> Catalogue() {
    std::lock_guard lock(g_lock);
    return g_catalogue;
}

void Undo(int steps) {
    for (int i = 0; i < steps; i++) Queue({Op::Undo});
}

std::vector<std::string> History() {
    size_t count = UndoCount();
    std::vector<UndoStep> steps;
    {
        std::lock_guard lock(g_lock);
        while (g_history.size() > count) g_history.pop_front();
        steps.assign(count - g_history.size(), UndoStep{0, "Edit"});
        steps.insert(steps.end(), g_history.begin(), g_history.end());
    }
    std::vector<UndoStep> extra = ExtraSteps();
    steps.insert(steps.end(), extra.begin(), extra.end());
    std::stable_sort(steps.begin(), steps.end(), [](const UndoStep& a, const UndoStep& b) { return a.stamp < b.stamp; });
    std::vector<std::string> out;
    for (const UndoStep& step : steps) out.push_back(step.label);
    return out;
}

void NoteMapChanged() { W8(kMapChanged, 1); }

uint64_t UndoStamp() { return ++g_stamp; }

void SetViewInput(const ViewInput& in) {
    g_apply = in.apply && ShellUp();
    std::lock_guard lock(g_lock);
    float look_x = g_view.look_x + in.look_x;
    float look_y = g_view.look_y + in.look_y;
    float wheel = g_view.wheel + in.wheel;
    g_view = in;
    g_view.look_x = look_x;
    g_view.look_y = look_y;
    g_view.wheel = wheel;
}

}  // namespace fcip::editor

namespace {

// port 0 after the keys: the left button holds what the active tool acts on
void EditorPad(input::Pad& pad) {
    if (!fcip::editor::ShellUp()) return;
    if (std::chrono::steady_clock::now().time_since_epoch().count() < g_pulse_until) pad.buttons |= g_pulse;
    if (!g_apply) return;
    uint16_t button = input::kA;
    if (CurrentTool() == fcip::editor::kTerrain) {
        int mode = int(R32(kTerrainTool + kTerrainMode));
        button = kBrushes[BrushFor(mode)].button;
    }
    pad.buttons |= button;
}

const bool g_registered = (fcip::g_editor_pad = EditorPad, true);

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_820C0340);
PPC_EXTERN_FUNC(__imp__sub_820CABB8);
PPC_EXTERN_FUNC(__imp__sub_820C6520);
PPC_EXTERN_FUNC(__imp__sub_820EFBB8);
PPC_EXTERN_FUNC(__imp__sub_820D0938);
PPC_EXTERN_FUNC(__imp__sub_820C4818);
PPC_EXTERN_FUNC(__imp__sub_820DFA90);
PPC_EXTERN_FUNC(__imp__sub_8210DCD8);
PPC_EXTERN_FUNC(__imp__sub_8210DDD8);
PPC_EXTERN_FUNC(__imp__sub_82313E70);

// an editor interface handler (r3) becomes active: the main editor's once a map is open, or again when the start
// menu closes, which cancels an Open Level that asked to leave
PPC_FUNC(sub_820C0340) {
    if (uint32_t(ctx.r[3]) == kEditorHandler && g_map_open) fcip::editor::ForgetLevelRequest();
    if (uint32_t(ctx.r[3]) == kEditorHandler && !g_map_open) {
        g_map_open = true;
        std::fill(std::begin(g_category_shown), std::end(g_category_shown), false);
    }
    __imp__sub_820C0340(ctx, base);
}

// the map manager becomes active: no map is open
PPC_FUNC(sub_820CABB8) {
    g_map_open = false;
    if (fcip::editor::g_solo) fcip::editor::LevelManagerShown();
    __imp__sub_820CABB8(ctx, base);
}

// the main editor's input, each frame while a map is open: the UI's commands first
PPC_FUNC(sub_820C6520) {
    if (g_map_open) RunCommands(ctx);
    __imp__sub_820C6520(ctx, base);
}

// the camera's update, from the active tool's; the tools act at the cursor it leaves
PPC_FUNC(sub_820EFBB8) {
    if (!fcip::editor::ShellUp()) return __imp__sub_820EFBB8(ctx, base);
    t_inject = TakeLook();
    bool raycast = R32(kCameraMode) == kCameraRaycast;
    float origin[3];  // the editor casts from its own camera
    if (raycast) t_inject.ray = MouseRay(origin, t_inject.dir);
    else RestoreOrbit();
    __imp__sub_820EFBB8(ctx, base);
    t_inject = {};
    if (!raycast) PlaceCursor(ctx);
}

// an input action's value (r3 the input map, r4 the action), the mouse added to the camera's
PPC_FUNC(sub_820D0938) {
    uint32_t action = uint32_t(ctx.r[4]);
    __imp__sub_820D0938(ctx, base);
    if (!t_inject.on) return;
    if (action == kActionLookX) ctx.f[1] += t_inject.look_x;
    if (action == kActionLookY) ctx.f[1] += t_inject.look_y;
}

// whether an input action is held: the camera pitches while the right button drags up or down
PPC_FUNC(sub_820C4818) {
    uint32_t action = uint32_t(ctx.r[4]);
    __imp__sub_820C4818(ctx, base);
    if (t_inject.pitch && action == kActionPitchHeld) ctx.r[3] = 1;
}

// a vector normalised (r3); the raycast camera's centre ray becomes the mouse's
PPC_FUNC(sub_820DFA90) {
    uint32_t v = uint32_t(ctx.r[3]);
    __imp__sub_820DFA90(ctx, base);
    if (!t_inject.ray) return;
    t_inject.ray = false;
    WriteVec(v, {t_inject.dir[0], t_inject.dir[1], t_inject.dir[2]});
}

// an undo command (r3) pushed; the stack keeps the last five
PPC_FUNC(sub_8210DCD8) {
    int tool = std::clamp(CurrentTool(), 0, fcip::editor::kToolCount - 1);
    std::string label = kToolNames[tool];
    if (tool == fcip::editor::kTerrain) label += std::string(": ") + kModeNames[R32(kTerrainTool + kTerrainMode) & 3];
    {
        std::lock_guard lock(g_lock);
        if (g_history.size() >= kUndoKept) g_history.pop_front();
        g_history.push_back({fcip::editor::UndoStamp(), label});
    }
    __imp__sub_8210DCD8(ctx, base);
}

// the newest undo command taken off the stack, 0 when empty
PPC_FUNC(sub_8210DDD8) {
    __imp__sub_8210DDD8(ctx, base);
    std::lock_guard lock(g_lock);
    if (ctx.r[3] && !g_history.empty()) g_history.pop_back();
}

// a page draws (r3); the console's editor pages stay hidden under the desktop UI, the solo map manager gets its rows
PPC_FUNC(sub_82313E70) {
    uint32_t page = uint32_t(ctx.r[3]);
    uint32_t name = R32(page + kPageName);
    if (name == kMapManagerPage && fcip::editor::g_solo) fcip::editor::UpdateLevelManager(ctx, page);
    bool console = std::find(std::begin(kConsolePages), std::end(kConsolePages), name) != std::end(kConsolePages);
    if (console && fcip::editor::ShellUp()) return;
    __imp__sub_82313E70(ctx, base);
}
