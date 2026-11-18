// ige.xex, the solo level editor: the Static Geometry tab shows, picks and places the level's static brushes, the
// rocks, huts and props baked into the terrain sectors. A sector (terrain +0x80, 0x160 each) keeps one CBrush per
// record of its sd%d.sdat (82354EA8 reads them, 82353000 makes them): +0xA8 the brushes, +0xBC one 8 byte state each.
// A brush's matrix is 16 floats at +0x5C, rows its local axes then its position, set through CBrush::SetMatrix
// (82368E78) as the sector load does, which also moves its world box (+0x28 min, +0x34 max); +0xA8 its type in
// Brush.lst. The editor's save writes every sector's brushes from these (82353C18), so a moved brush saves where it
// stands. A sector's lists are only built as it loads, so a brush is added or removed in its file as the save writes
// it: an added brush is drawn as a box until the level opens again, a removed one goes out of sight at once, and the
// save appends and drops their 72 byte records in sd%d.sdat (the save's layout: 0, type, 0, flags, the matrix's
// columns with the position last in each, two view ratios).
#include "leveleditor/static_geometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/edge_walls.h"
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

// Brush.lst: "CRY", 1, 4, the count, then each type's flag, box min and max and model CRC
constexpr size_t kListCount = 11;
constexpr size_t kListTypes = 15;
constexpr size_t kListTypeBytes = 32;
constexpr size_t kTypeBox = 4;
// sd%d.sdat: the brush count, then the records after the header
constexpr size_t kSectorFileBrushCount = 0x14;
constexpr size_t kSectorFileBrushes = 56;
constexpr size_t kRecordBytes = 72;
constexpr size_t kRecordType = 4;
constexpr size_t kRecordFlags = 12;
constexpr size_t kRecordMatrix = 16;
constexpr size_t kRecordZ = 60;
constexpr size_t kRecordRatios = 64;
constexpr uint32_t kDefaultFlags = 22;  // most of 01_BEACH's buildings'
constexpr float kRemovedZ = -10000.0f;  // a removed brush waits here until the save drops it
constexpr float kSectorMetres = 64.0f;

constexpr float kRadians = 3.14159265f / 180.0f;
constexpr float kShownMetres = 60.0f;  // boxes drawn around the camera
constexpr ImU32 kBoxColour = IM_COL32(150, 200, 255, 110);
constexpr ImU32 kPickedColour = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kAddedColour = IM_COL32(120, 255, 140, 220);

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

// a brush added since the level opened, written into its sector's file by the save
struct AddedBrush {
    int type = 0;
    Placement place;
};
// a Brush.lst type's box
struct BrushType {
    float min[3] = {};
    float max[3] = {};
};

std::mutex g_lock;
std::shared_ptr<const StaticBrushes> g_brushes = std::make_shared<StaticBrushes>();
std::vector<AddedBrush> g_added;
std::vector<BrushType> g_types;
std::vector<Edit> g_edits;
std::atomic<bool> g_stale{true};
std::atomic<uint32_t> g_picked{0};  // the picked brush's node
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown_terrain = 0;
std::map<uint32_t, BrushFootprint> g_opened;  // each brush's footprint as the level opened
// window thread
bool g_show = false;
bool g_picking = false;
int g_add_type = 0;

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

bool Removed(const StaticBrush& b) { return b.matrix[14] <= kRemovedZ + 1; }

BrushFootprint FootprintOf(const StaticBrush& b) { return {b.min[0], b.min[1], b.max[0], b.max[1]}; }

std::vector<BrushType> ReadTypes(PPCContext& ctx) {
    std::vector<uint8_t> list;
    std::vector<BrushType> types;
    if (!ReadPackFile(ctx, "levels\\" + StartedLevel() + "\\Brush.lst", list) || list.size() < kListTypes) {
        return types;
    }
    uint32_t count = 0;
    std::memcpy(&count, list.data() + kListCount, 4);
    for (uint32_t i = 0; i < count && kListTypes + (i + 1) * kListTypeBytes <= list.size(); i++) {
        BrushType t;
        std::memcpy(t.min, list.data() + kListTypes + i * kListTypeBytes + kTypeBox, 12);
        std::memcpy(t.max, list.data() + kListTypes + i * kListTypeBytes + kTypeBox + 12, 12);
        types.push_back(t);
    }
    return types;
}

}  // namespace

namespace {

std::array<float, kMatrixFloats> MatrixOf(const Placement& p);

// the type's box turned and placed by the matrix: its eight corners, and the world box round them
void PlacedCorners(const BrushType& t, const std::array<float, kMatrixFloats>& m, float corners[8][3]) {
    for (int c = 0; c < 8; c++) {
        float local[3] = {c & 1 ? t.max[0] : t.min[0], c & 2 ? t.max[1] : t.min[1], c & 4 ? t.max[2] : t.min[2]};
        for (int i = 0; i < 3; i++) {
            corners[c][i] = local[0] * m[i] + local[1] * m[4 + i] + local[2] * m[8 + i] + m[12 + i];
        }
    }
}

BrushFootprint AddedFootprint(const AddedBrush& a, const std::vector<BrushType>& types) {
    if (a.type < 0 || size_t(a.type) >= types.size()) return {};
    float corners[8][3];
    PlacedCorners(types[size_t(a.type)], MatrixOf(a.place), corners);
    BrushFootprint f{corners[0][0], corners[0][1], corners[0][0], corners[0][1]};
    for (const auto& c : corners) {
        f.min_x = std::min(f.min_x, c[0]);
        f.min_y = std::min(f.min_y, c[1]);
        f.max_x = std::max(f.max_x, c[0]);
        f.max_y = std::max(f.max_y, c[1]);
    }
    return f;
}

std::vector<BrushFootprint> AddedFootprints() {
    std::lock_guard lock(g_lock);
    std::vector<BrushFootprint> out;
    for (const AddedBrush& a : g_added) out.push_back(AddedFootprint(a, g_types));
    return out;
}

}  // namespace

std::vector<BrushFootprint> fcip::editor::BrushFootprints() {
    std::vector<BrushFootprint> out;
    for (const StaticBrush& b : *ReadBrushes(Terrain())) {
        if (!Removed(b)) out.push_back(FootprintOf(b));
    }
    for (const BrushFootprint& f : AddedFootprints()) out.push_back(f);
    return out;
}

BrushChanges fcip::editor::ChangedBrushes() {
    constexpr float kMoved = 0.05f;  // metres
    BrushChanges out;
    for (const StaticBrush& b : *ReadBrushes(Terrain())) {
        auto opened = g_opened.find(b.node);
        BrushFootprint now = FootprintOf(b);
        if (opened == g_opened.end()) continue;
        const BrushFootprint& was = opened->second;
        bool moved = std::fabs(now.min_x - was.min_x) > kMoved || std::fabs(now.min_y - was.min_y) > kMoved ||
                     std::fabs(now.max_x - was.max_x) > kMoved || std::fabs(now.max_y - was.max_y) > kMoved;
        if (!moved && !Removed(b)) continue;
        out.cleared.push_back(was);
        if (!Removed(b)) out.placed.push_back(now);
    }
    for (const BrushFootprint& f : AddedFootprints()) out.placed.push_back(f);
    return out;
}

namespace {

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
        if (Removed(b)) continue;
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

// a placement's rows edited; true with the edited placement when one changed
bool EditPlacement(Placement& edited) {
    float label_w = ImGui::GetFontSize() * 8;
    struct Row {
        const char* label;
        float* values;
    };
    Row rows[] = {{"Position", edited.position}, {"Angles", edited.angles}, {"Scale", edited.scale}};
    bool changed = false;
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
        changed = true;
    }
    return changed;
}

void AddBrush(PPCContext& ctx, const AddedBrush& a) {
    {
        std::lock_guard lock(g_lock);
        g_added.push_back(a);
    }
    NoteMapChanged();
    PushUndo(ctx, "Add a static brush", [](PPCContext&) {
        std::lock_guard lock(g_lock);
        if (!g_added.empty()) g_added.pop_back();
    });
}

void SetAdded(PPCContext& ctx, size_t index, const Placement& place) {
    Placement old;
    {
        std::lock_guard lock(g_lock);
        if (index >= g_added.size()) return;
        old = g_added[index].place;
        g_added[index].place = place;
    }
    NoteMapChanged();
    PushUndo(ctx, "Place an added brush", [index, old](PPCContext&) {
        std::lock_guard lock(g_lock);
        if (index < g_added.size()) g_added[index].place = old;
    });
}

void DropAdded(PPCContext& ctx, size_t index) {
    AddedBrush dropped;
    {
        std::lock_guard lock(g_lock);
        if (index >= g_added.size()) return;
        dropped = g_added[index];
        g_added.erase(g_added.begin() + std::ptrdiff_t(index));
    }
    NoteMapChanged();
    PushUndo(ctx, "Remove an added brush", [index, dropped](PPCContext&) {
        std::lock_guard lock(g_lock);
        g_added.insert(g_added.begin() + std::ptrdiff_t(std::min(index, g_added.size())), dropped);
    });
}

std::string TypeLabel(const std::vector<BrushType>& types, int type) {
    char text[64];
    if (type < 0 || size_t(type) >= types.size()) return "Type " + std::to_string(type);
    const BrushType& t = types[size_t(type)];
    std::snprintf(text, sizeof(text), "Type %d, %.0f x %.0f x %.0f m", type, t.max[0] - t.min[0], t.max[1] - t.min[1],
                  t.max[2] - t.min[2]);
    return text;
}

Placement AtCursor() {
    Placement p;
    State tools = Read();
    for (int i = 0; i < 3; i++) p.position[i] = tools.cursor[i];
    return p;
}

void DrawAdding(const StaticBrush* picked) {
    std::vector<BrushType> types;
    std::vector<AddedBrush> added;
    {
        std::lock_guard lock(g_lock);
        types = g_types;
        added = g_added;
    }
    ImGui::SeparatorText("Add a brush");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##type", TypeLabel(types, g_add_type).c_str())) {
        for (int i = 0; i < int(types.size()); i++) {
            if (ImGui::Selectable(TypeLabel(types, i).c_str(), i == g_add_type)) g_add_type = i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Add at the tool cursor") && g_add_type < int(types.size())) {
        AddedBrush a{g_add_type, AtCursor()};
        Queue([a](PPCContext& ctx) { AddBrush(ctx, a); });
    }
    if (picked && !Removed(*picked)) {
        ImGui::SameLine();
        if (ImGui::Button("Add a copy of the picked brush")) {
            AddedBrush a{picked->type, PlacementOf(picked->matrix)};
            Placement at = AtCursor();
            std::copy(std::begin(at.position), std::end(at.position), a.place.position);
            Queue([a](PPCContext& ctx) { AddBrush(ctx, a); });
        }
    }
    if (added.empty()) return;
    ImGui::TextDisabled("Added brushes save into their sectors and are solid once the level opens again");
    for (size_t i = 0; i < added.size(); i++) {
        ImGui::PushID(int(i));
        ImGui::TextUnformatted(TypeLabel(types, added[i].type).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) Queue([i](PPCContext& ctx) { DropAdded(ctx, i); });
        Placement edited = added[i].place;
        if (EditPlacement(edited)) Queue([i, edited](PPCContext& ctx) { SetAdded(ctx, i, edited); });
        ImGui::PopID();
    }
}

void DrawPlacement(const StaticBrush& b) {
    Placement edited = PlacementOf(b.matrix);
    if (!EditPlacement(edited)) return;
    uint32_t node = b.node;
    std::array<float, kMatrixFloats> matrix = MatrixOf(edited);
    Queue([node, matrix](PPCContext& ctx) { SetMatrix(ctx, node, matrix, true); });
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
    UpdateEdgeWalls(ctx, terrain != g_shown_terrain);
    if (terrain == g_shown_terrain && !g_stale) return;
    std::shared_ptr<const StaticBrushes> brushes = ReadBrushes(terrain);
    if (terrain != g_shown_terrain) {
        std::vector<BrushType> types = ReadTypes(ctx);
        g_opened.clear();
        for (const StaticBrush& b : *brushes) g_opened[b.node] = FootprintOf(b);
        std::lock_guard lock(g_lock);
        g_types = std::move(types);
        g_added.clear();
    }
    g_shown_terrain = terrain;
    g_stale = false;
    std::lock_guard lock(g_lock);
    g_brushes = std::move(brushes);
}

void fcip::editor::WriteBrushEdits(const std::filesystem::path& folder) {
    std::vector<AddedBrush> added;
    {
        std::lock_guard lock(g_lock);
        added = g_added;
    }
    uint32_t side = R32(SYM_Terrain_SectorsPerSide);
    std::vector<std::vector<uint8_t>> files(size_t(side) * side);
    std::map<uint32_t, std::vector<uint8_t>> samples;  // a record of each type the level has, for its flags and ratios
    for (uint32_t i = 0; i < files.size(); i++) {
        char name[32];
        std::snprintf(name, sizeof(name), "sd%u.sdat", i);
        std::ifstream in(folder / "sdat" / name, std::ios::binary);
        files[i].assign(std::istreambuf_iterator<char>(in), {});
        std::vector<uint8_t>& f = files[i];
        if (f.size() < kSectorFileBrushes) continue;
        uint32_t count = 0;
        std::memcpy(&count, f.data() + kSectorFileBrushCount, 4);
        for (uint32_t r = 0; r < count && kSectorFileBrushes + (r + 1) * kRecordBytes <= f.size(); r++) {
            const uint8_t* record = f.data() + kSectorFileBrushes + r * kRecordBytes;
            uint32_t type = 0;
            std::memcpy(&type, record + kRecordType, 4);
            samples.emplace(type, std::vector<uint8_t>(record, record + kRecordBytes));
        }
    }
    std::vector<std::vector<uint8_t>> appended(files.size());
    for (const AddedBrush& a : added) {
        std::array<float, kMatrixFloats> m = MatrixOf(a.place);
        int sx = std::clamp(int(m[12] / kSectorMetres), 0, int(side) - 1);
        int sy = std::clamp(int(m[13] / kSectorMetres), 0, int(side) - 1);
        std::vector<uint8_t> record(kRecordBytes);
        uint32_t type = uint32_t(a.type);
        uint32_t flags = kDefaultFlags;
        float ratios[2] = {1.0f, 1.0f};
        auto sample = samples.find(type);
        if (sample != samples.end()) {
            std::memcpy(&flags, sample->second.data() + kRecordFlags, 4);
            std::memcpy(ratios, sample->second.data() + kRecordRatios, 8);
        }
        std::memcpy(record.data() + kRecordType, &type, 4);
        std::memcpy(record.data() + kRecordFlags, &flags, 4);
        // each row is a column of the brush's matrix, then the position's part
        for (int r = 0; r < 3; r++) {
            float row[4] = {m[r], m[4 + r], m[8 + r], m[12 + r]};
            std::memcpy(record.data() + kRecordMatrix + size_t(r) * 16, row, 16);
        }
        std::memcpy(record.data() + kRecordRatios, ratios, 8);
        std::vector<uint8_t>& to = appended[size_t(sy) * side + size_t(sx)];
        to.insert(to.end(), record.begin(), record.end());
    }
    for (uint32_t i = 0; i < files.size(); i++) {
        std::vector<uint8_t>& f = files[i];
        if (f.size() < kSectorFileBrushes) continue;
        uint32_t count = 0;
        std::memcpy(&count, f.data() + kSectorFileBrushCount, 4);
        size_t end = kSectorFileBrushes + size_t(count) * kRecordBytes;
        if (end > f.size()) continue;
        // an added brush an earlier save appended is in the file already where the save kept it
        auto appended_before = [&](size_t at) {
            for (size_t a = 0; a < appended[i].size(); a += kRecordBytes) {
                const uint8_t* mine = appended[i].data() + a;
                const uint8_t* theirs = f.data() + at;
                bool same_type = std::memcmp(mine + kRecordType, theirs + kRecordType, 4) == 0;
                size_t matrix_bytes = kRecordRatios - kRecordMatrix;
                bool same_place = std::memcmp(mine + kRecordMatrix, theirs + kRecordMatrix, matrix_bytes) == 0;
                if (same_type && same_place) return true;
            }
            return false;
        };
        std::vector<uint8_t> kept;
        for (size_t at = kSectorFileBrushes; at < end; at += kRecordBytes) {
            float z = 0;
            std::memcpy(&z, f.data() + at + kRecordZ, 4);
            if (z > kRemovedZ + 1 && !appended_before(at)) {
                kept.insert(kept.end(), f.begin() + at, f.begin() + at + kRecordBytes);
            }
        }
        kept.insert(kept.end(), appended[i].begin(), appended[i].end());
        if (kept.size() == end - kSectorFileBrushes && appended[i].empty()) continue;
        uint32_t now = uint32_t(kept.size() / kRecordBytes);
        std::vector<uint8_t> out(f.begin(), f.begin() + kSectorFileBrushes);
        std::memcpy(out.data() + kSectorFileBrushCount, &now, 4);
        out.insert(out.end(), kept.begin(), kept.end());
        out.insert(out.end(), f.begin() + end, f.end());
        char name[32];
        std::snprintf(name, sizeof(name), "sd%u.sdat", i);
        std::ofstream(folder / "sdat" / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    }
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
            if (Removed(b)) continue;
            float dx = (b.min[0] + b.max[0]) * 0.5f - v.eye[0];
            float dy = (b.min[1] + b.max[1]) * 0.5f - v.eye[1];
            if (dx * dx + dy * dy < kShownMetres * kShownMetres) AxisBox(v, b.min, b.max, kBoxColour);
        }
    }
    if (const StaticBrush* b = PickedIn(*brushes)) AxisBox(v, b->min, b->max, kPickedColour);
    std::lock_guard lock(g_lock);
    for (const AddedBrush& a : g_added) {
        if (a.type < 0 || size_t(a.type) >= g_types.size()) continue;
        float corners[8][3];
        PlacedCorners(g_types[size_t(a.type)], MatrixOf(a.place), corners);
        BoxEdges(v, corners, kAddedColour);
    }
    DrawEdgeWalls(v);
}

void fcip::editor::DrawStaticGeometryPanel() {
    ImGui::TextUnformatted("STATIC GEOMETRY");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("The brushes baked into the terrain sectors, saved where they stand");
    ImGui::PopStyleColor();
    ImGui::Separator();
    std::shared_ptr<const StaticBrushes> brushes = ReadSnapshot();
    size_t removed = size_t(std::count_if(brushes->begin(), brushes->end(), Removed));
    ImGui::Text("%zu brushes, %zu removed until the save", brushes->size() - removed, removed);
    ImGui::Checkbox("Show boxes near the camera", &g_show);
    ImGui::Checkbox("Pick with a click in the view", &g_picking);
    ImGui::Separator();
    const StaticBrush* b = PickedIn(*brushes);
    if (!b || Removed(*b)) {
        ImGui::TextUnformatted("Pick a brush in the view.");
    } else {
        ImGui::Text("Brush.lst type %d, sector %d", b->type, b->sector);
        DrawPlacement(*b);
        if (ImGui::Button("Remove the picked brush")) {
            uint32_t node = b->node;
            Placement gone = PlacementOf(b->matrix);
            gone.position[2] = kRemovedZ;
            std::array<float, kMatrixFloats> matrix = MatrixOf(gone);
            Queue([node, matrix](PPCContext& ctx) { SetMatrix(ctx, node, matrix, true); });
        }
    }
    DrawAdding(b);
    DrawEdgeWallsPanel();
}
