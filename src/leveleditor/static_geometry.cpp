// ige.xex, the solo level editor: the Static Geometry tab shows, picks and places the level's static brushes, the
// rocks, huts and props baked into the terrain sectors. A sector (terrain +0x80, 0x160 each) keeps one CBrush per
// record of its sd%d.sdat (82354EA8 reads them, 82353000 makes them): +0xA8 the brushes, +0xBC one 8 byte state each.
// A brush's matrix is 16 floats at +0x5C, rows its local axes then its position, set through CBrush::SetMatrix
// (82368E78) as the sector load does, which also moves its world box (+0x28 min, +0x34 max); +0xA8 its type in
// Brush.lst. The editor's save writes every sector's brushes from these (82353C18), so a moved brush saves where it
// stands. Brushes are not added or removed here: a sector's lists are only built as it loads.
#include "leveleditor/static_geometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/fields.h"
#include "leveleditor/guest.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// the 3D engine's terrain and its sectors
constexpr uint32_t kEngineTerrain = 0x364;
constexpr uint32_t kTerrainSectors = 0x80;
constexpr uint32_t kSectorBytes = 0x160;
constexpr uint32_t kSectorBrushes = 0xA8;
constexpr uint32_t kSectorStateFirst = 0xBC;
constexpr uint32_t kSectorStateLast = 0xC0;
constexpr uint32_t kStateBytes = 8;
// CBrush
constexpr uint32_t kBrushBoxMin = 0x28;
constexpr uint32_t kBrushBoxMax = 0x34;
constexpr uint32_t kBrushMatrix = 0x5C;
constexpr uint32_t kBrushType = 0xA8;
constexpr int kMatrixFloats = 16;

constexpr float kRadians = 3.14159265f / 180.0f;
constexpr float kShownMetres = 60.0f;  // boxes drawn around the camera
constexpr ImU32 kBoxColour = IM_COL32(150, 200, 255, 110);
constexpr ImU32 kPickedColour = IM_COL32(255, 255, 255, 255);

struct StaticBrush {
    uint32_t node = 0;
    int sector = 0;
    int type = 0;
    float matrix[kMatrixFloats] = {};
    float min[3] = {};
    float max[3] = {};
};
using StaticBrushes = std::vector<StaticBrush>;
using Edit = std::function<void(PPCContext&)>;
// a brush placed as the tab edits it: position, angles in degrees as entities keep them, scale per axis
struct Placement {
    float position[3] = {};
    float angles[3] = {};
    float scale[3] = {1, 1, 1};
};

std::mutex g_lock;
std::shared_ptr<const StaticBrushes> g_brushes = std::make_shared<StaticBrushes>();
std::vector<Edit> g_edits;
std::atomic<bool> g_stale{true};
std::atomic<uint32_t> g_picked{0};  // the picked brush's node
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown_terrain = 0;
// window thread
bool g_show = false;
bool g_picking = false;

float ReadFloat(uint32_t at) { return std::bit_cast<float>(R32(at)); }

// Editor thread

uint32_t Terrain() {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    return engine ? R32(engine + kEngineTerrain) : 0;
}

StaticBrush ReadBrush(uint32_t node, int sector) {
    StaticBrush b;
    b.node = node;
    b.sector = sector;
    b.type = int(R32(node + kBrushType));
    for (int i = 0; i < kMatrixFloats; i++) b.matrix[i] = ReadFloat(node + kBrushMatrix + uint32_t(i) * 4);
    for (int i = 0; i < 3; i++) {
        b.min[i] = ReadFloat(node + kBrushBoxMin + uint32_t(i) * 4);
        b.max[i] = ReadFloat(node + kBrushBoxMax + uint32_t(i) * 4);
    }
    return b;
}

std::shared_ptr<const StaticBrushes> ReadBrushes(uint32_t terrain) {
    auto out = std::make_shared<StaticBrushes>();
    uint32_t sectors = terrain ? R32(terrain + kTerrainSectors) : 0;
    uint32_t side = R32(SYM_Terrain_SectorsPerSide);
    for (uint32_t s = 0; sectors && s < side * side; s++) {
        uint32_t sector = sectors + s * kSectorBytes;
        uint32_t first = R32(sector + kSectorStateFirst);
        uint32_t count = first ? (R32(sector + kSectorStateLast) - first) / kStateBytes : 0;
        uint32_t nodes = R32(sector + kSectorBrushes);
        for (uint32_t i = 0; nodes && i < count; i++) {
            if (uint32_t node = R32(nodes + i * 4)) out->push_back(ReadBrush(node, int(s)));
        }
    }
    return out;
}

// the brush takes the matrix as the sector load sets it; one step of undo
void SetMatrix(PPCContext& ctx, uint32_t node, const std::array<float, kMatrixFloats>& matrix, bool undoable) {
    if (!g_scratch) g_scratch = mem::AllocSystem(kMatrixFloats * 4);
    if (!g_scratch) return;
    std::array<float, kMatrixFloats> old;
    for (int i = 0; i < kMatrixFloats; i++) old[i] = ReadFloat(node + kBrushMatrix + uint32_t(i) * 4);
    for (int i = 0; i < kMatrixFloats; i++) W32(g_scratch + uint32_t(i) * 4, std::bit_cast<uint32_t>(matrix[i]));
    CallGuest(ctx, SYM_Brush_SetMatrix, {node, g_scratch});
    g_stale = true;
    NoteMapChanged();
    if (!undoable) return;
    PushUndo(ctx, "Place a static brush", [node, old](PPCContext& ctx) { SetMatrix(ctx, node, old, false); });
}

// Placement and matrix

// the inverse of AxesFromAngles: x = (cz cy, sz cy, -sy), y.z = cy sx, z.z = cy cx
Placement PlacementOf(const float m[kMatrixFloats]) {
    Placement p;
    float axes[3][3];
    for (int r = 0; r < 3; r++) {
        float length = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
        p.scale[r] = length;
        for (int c = 0; c < 3; c++) axes[r][c] = length > 0 ? m[r * 4 + c] / length : 0.0f;
    }
    p.angles[1] = std::asin(std::clamp(-axes[0][2], -1.0f, 1.0f)) / kRadians;
    p.angles[2] = std::atan2(axes[0][1], axes[0][0]) / kRadians;
    p.angles[0] = std::atan2(axes[1][2], axes[2][2]) / kRadians;
    for (int i = 0; i < 3; i++) p.position[i] = m[12 + i];
    return p;
}

std::array<float, kMatrixFloats> MatrixOf(const Placement& p) {
    float axes[3][3];
    AxesFromAngles(p.angles, axes);
    std::array<float, kMatrixFloats> m{};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) m[r * 4 + c] = axes[r][c] * p.scale[r];
    }
    for (int i = 0; i < 3; i++) m[12 + i] = p.position[i];
    m[15] = 1.0f;
    return m;
}

// Panel and view, window thread

std::shared_ptr<const StaticBrushes> ReadSnapshot() {
    std::lock_guard lock(g_lock);
    return g_brushes;
}

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

const StaticBrush* PickedIn(const StaticBrushes& brushes) {
    uint32_t picked = g_picked;
    for (const StaticBrush& b : brushes) {
        if (b.node == picked) return &b;
    }
    return nullptr;
}

// where the ray enters the box, false when it misses
bool RayHitsBox(const float origin[3], const float dir[3], const float min[3], const float max[3], float& t) {
    float near = 0.0f;
    float far = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (origin[i] < min[i] || origin[i] > max[i]) return false;
            continue;
        }
        float a = (min[i] - origin[i]) / dir[i];
        float b = (max[i] - origin[i]) / dir[i];
        near = std::max(near, std::min(a, b));
        far = std::min(far, std::max(a, b));
    }
    t = near;
    return near <= far;
}

float Volume(const StaticBrush& b) { return (b.max[0] - b.min[0]) * (b.max[1] - b.min[1]) * (b.max[2] - b.min[2]); }

// the brush box under the mouse the ray enters first, 0 for none; boxes holding the camera only when the ray enters
// no other, the smallest of them
uint32_t BrushUnder(const View& v, ImVec2 m) {
    float origin[3];
    float dir[3];
    if (!MouseRay(v, m, origin, dir)) return 0;
    uint32_t entered = 0;
    float entered_t = 1e30f;
    uint32_t inside = 0;
    float inside_volume = 1e30f;
    for (const StaticBrush& b : *ReadSnapshot()) {
        float t = 0;
        if (!RayHitsBox(origin, dir, b.min, b.max, t)) continue;
        if (t > 0 && t < entered_t) {
            entered_t = t;
            entered = b.node;
        } else if (t <= 0 && Volume(b) < inside_volume) {
            inside_volume = Volume(b);
            inside = b.node;
        }
    }
    return entered ? entered : inside;
}

void DrawPlacement(const StaticBrush& b) {
    Placement p = PlacementOf(b.matrix);
    float label_w = ImGui::GetFontSize() * 8;
    struct Row {
        const char* label;
        float* values;
    };
    Placement edited = p;
    Row rows[] = {{"Position", edited.position}, {"Angles", edited.angles}, {"Scale", edited.scale}};
    for (const Row& r : rows) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r.label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        char text[96];
        std::snprintf(text, sizeof(text), "%g,%g,%g", r.values[0], r.values[1], r.values[2]);
        std::string out;
        if (!DrawField(r.label, FieldKind::Vector, text, out)) continue;
        std::sscanf(out.c_str(), "%f,%f,%f", &r.values[0], &r.values[1], &r.values[2]);
        uint32_t node = b.node;
        std::array<float, kMatrixFloats> matrix = MatrixOf(edited);
        Queue([node, matrix](PPCContext& ctx) { SetMatrix(ctx, node, matrix, true); });
    }
}

}  // namespace

void fcip::editor::UpdateStaticGeometry(PPCContext& ctx) {
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    uint32_t terrain = Terrain();
    if (terrain == g_shown_terrain && !g_stale) return;
    std::shared_ptr<const StaticBrushes> brushes = ReadBrushes(terrain);
    g_shown_terrain = terrain;
    g_stale = false;
    std::lock_guard lock(g_lock);
    g_brushes = std::move(brushes);
}

bool fcip::editor::BrushMouse(ImVec2 mouse, bool pressed) {
    View v;
    if (!g_picking || !pressed || !ReadView(v)) return false;
    g_picked = BrushUnder(v, mouse);
    return true;
}

void fcip::editor::DrawStaticGeometry(const View& v) {
    std::shared_ptr<const StaticBrushes> brushes = ReadSnapshot();
    if (g_show && v.has_eye) {
        for (const StaticBrush& b : *brushes) {
            float dx = (b.min[0] + b.max[0]) * 0.5f - v.eye[0];
            float dy = (b.min[1] + b.max[1]) * 0.5f - v.eye[1];
            if (dx * dx + dy * dy < kShownMetres * kShownMetres) AxisBox(v, b.min, b.max, kBoxColour);
        }
    }
    if (const StaticBrush* b = PickedIn(*brushes)) AxisBox(v, b->min, b->max, kPickedColour);
}

void fcip::editor::DrawStaticGeometryPanel() {
    ImGui::TextUnformatted("STATIC GEOMETRY");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("The brushes baked into the terrain sectors, saved where they stand");
    ImGui::PopStyleColor();
    ImGui::Separator();
    std::shared_ptr<const StaticBrushes> brushes = ReadSnapshot();
    ImGui::Text("%zu brushes", brushes->size());
    ImGui::Checkbox("Show boxes near the camera", &g_show);
    ImGui::Checkbox("Pick with a click in the view", &g_picking);
    ImGui::Separator();
    const StaticBrush* b = PickedIn(*brushes);
    if (!b) {
        ImGui::TextUnformatted("Pick a brush in the view.");
        return;
    }
    ImGui::Text("Brush.lst type %d, sector %d", b->type, b->sector);
    DrawPlacement(*b);
}
