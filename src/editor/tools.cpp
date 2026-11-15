// ige.xex: the map editor's tools, camera and undo driven from the desktop UI. Commands queued by the UI run on the
// editor's thread. The mouse turns and zooms the camera through the editor's own look inputs, moves the tool cursor
// to where it points, and holds the pad button the active tool acts on.
#include "editor/tools.h"

#include <algorithm>
#include <atomic>
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
constexpr uint32_t kSetHeight = 0x828C4A54;
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

enum class Op { SelectTool, TerrainMode, RadiusStep, SetHeight, Texture, SquareCursor, Undo };
struct Command {
    Op op;
    int value = 0;
    float amount = 0.0f;
};

struct Vec {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool operator==(const Vec&) const = default;
};

std::atomic<bool> g_map_open{false};  // from the main editor showing until the map manager does
std::atomic<int> g_brush{fcip::editor::kRaise};
std::atomic<bool> g_apply{false};

std::mutex g_lock;
std::vector<Command> g_commands;
std::deque<std::string> g_history;  // labels beside the editor's undo stack
fcip::editor::ViewInput g_view;      // the latest, deltas summed until the camera takes them

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

void RunUndo(PPCContext& ctx) {
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
            WF(kSetHeight, c.amount);
            Caption(ctx, kHeightCaption);
            break;
        case Op::Texture:
            W32(kTerrainTool + kTerrainTexture, uint32_t(c.value));
            Caption(ctx, kTextureCaption);
            break;
        case Op::SquareCursor: W32(kSquareCursor, uint32_t(c.value)); break;
        case Op::Undo: RunUndo(ctx); break;
    }
}

void RunCommands(PPCContext& ctx) {
    std::vector<Command> commands;
    {
        std::lock_guard lock(g_lock);
        commands.swap(g_commands);
    }
    for (const Command& c : commands) Run(ctx, c);
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

bool ShellUp() { return g_map_open && R32(kEditorHandler + kHandlerMode) == kModeEditing; }

State Read() {
    State s;
    s.tool = std::clamp(CurrentTool(), 0, kToolCount - 1);
    int mode = int(R32(kTerrainTool + kTerrainMode));
    s.brush = BrushFor(mode);
    s.radius_step = std::clamp(int(R32(kTerrainTool + kTerrainRadiusStep)), 0, kRadiusSteps - 1);
    s.radius = GF(kRadius);
    s.set_height = GF(kSetHeight);
    s.texture = std::clamp(int(R32(kTerrainTool + kTerrainTexture)), 0, kTextureCount - 1);
    s.square_cursor = R32(kSquareCursor) != 0;
    Vec c = ReadVec(kToolCursor);
    s.cursor[0] = c.x;
    s.cursor[1] = c.y;
    s.cursor[2] = c.z;
    s.undo_count = UndoCount();
    s.applying = g_apply;
    return s;
}

const char* ToolName(int tool) { return kToolNames[std::clamp(tool, 0, kToolCount - 1)]; }
const char* BrushName(int brush) { return kBrushes[std::clamp(brush, 0, kBrushCount - 1)].name; }
const char* TextureName(int texture) { return kTextureNames[std::clamp(texture, 0, kTextureCount - 1)]; }
float RadiusMetres(int step) { return GF(kRadiusTable + uint32_t(std::clamp(step, 0, kRadiusSteps - 1)) * 4); }

void SelectTool(int tool) { Queue({Op::SelectTool, std::clamp(tool, 0, kToolCount - 1)}); }

void SelectBrush(int brush) {
    brush = std::clamp(brush, 0, kBrushCount - 1);
    g_brush = brush;
    Queue({Op::SelectTool, kTerrain});
    if (kBrushes[brush].mode >= 0) Queue({Op::TerrainMode, kBrushes[brush].mode});
}

void SetRadiusStep(int step) { Queue({Op::RadiusStep, std::clamp(step, 0, kRadiusSteps - 1)}); }
void SetSetHeight(float height) { Queue({Op::SetHeight, 0, std::clamp(height, 0.0f, kMaxSetHeight)}); }
void SetTexture(int texture) { Queue({Op::Texture, std::clamp(texture, 0, kTextureCount - 1)}); }
void SetSquareCursor(bool square) { Queue({Op::SquareCursor, square ? 1 : 0}); }

void Undo(int steps) {
    for (int i = 0; i < steps; i++) Queue({Op::Undo});
}

std::vector<std::string> History() {
    size_t count = UndoCount();
    std::lock_guard lock(g_lock);
    while (g_history.size() > count) g_history.pop_front();
    std::vector<std::string> out(count - g_history.size(), "Edit");
    out.insert(out.end(), g_history.begin(), g_history.end());
    return out;
}

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
    if (!g_apply || !fcip::editor::ShellUp()) return;
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

// an editor interface handler (r3) becomes active: the main editor's once a map is open
PPC_FUNC(sub_820C0340) {
    if (uint32_t(ctx.r[3]) == kEditorHandler) g_map_open = true;
    __imp__sub_820C0340(ctx, base);
}

// the map manager becomes active: no map is open
PPC_FUNC(sub_820CABB8) {
    g_map_open = false;
    __imp__sub_820CABB8(ctx, base);
}

// the main editor's input, each frame while editing: the UI's commands first
PPC_FUNC(sub_820C6520) {
    if (fcip::editor::ShellUp()) RunCommands(ctx);
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
        g_history.push_back(label);
    }
    __imp__sub_8210DCD8(ctx, base);
}

// the newest undo command taken off the stack, 0 when empty
PPC_FUNC(sub_8210DDD8) {
    __imp__sub_8210DDD8(ctx, base);
    std::lock_guard lock(g_lock);
    if (ctx.r[3] && !g_history.empty()) g_history.pop_back();
}
