// ige.xex, the solo level editor: what the level uses to cut what it draws. The 3D engine reads the mission's VisArea
// and Portal objects itself (default.xex 825FBBC8): a portal is one whose Name holds "portal", each is its points,
// every point from its own height up Height, with its AmbientColor, sun, sky, terrain and water flags. The terrain is
// kept in 64 m sectors (82324AA8 indexes them row by row), drawn as a grid along the ground with each sector's
// number. A ViewDist entity sets the view distance to MaxViewDist while the player is inside the areas naming it.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/tools.h"
#include "kernel/kernel.h"
#include "leveleditor/markers.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr float kSectorMetres = 64.0f;
constexpr float kGridStep = 8.0f;  // metres between the grid's ground samples
constexpr int kStepsPerSector = int(kSectorMetres / kGridStep);

constexpr ImU32 kVisAreaColour = IM_COL32(70, 200, 200, 255);
constexpr ImU32 kPortalColour = IM_COL32(150, 240, 255, 255);
constexpr ImU32 kGridColour = IM_COL32(230, 230, 230, 70);
constexpr ImU32 kViewDistColour = IM_COL32(190, 230, 90, 255);

// the sector grid along the ground: lines x = k * 64 then y = k * 64, a height per 8 m sample
struct Grid {
    int side = 0;  // sectors per side
    std::vector<std::vector<float>> heights;
};

std::mutex g_lock;
std::shared_ptr<const Grid> g_grid = std::make_shared<Grid>();
Grid g_building;  // editor thread
size_t g_next_line = 0;

std::shared_ptr<const Grid> CurrentGrid() {
    std::lock_guard lock(g_lock);
    return g_grid;
}

bool IsPortal(const AiObject& o) {
    std::string name = o.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return name.find("portal") != std::string::npos;
}

void DrawVisArea(const View& v, const AiObject& o) {
    if (o.points.size() < 2) return;
    ImU32 colour = IsPortal(o) ? kPortalColour : kVisAreaColour;
    float height = std::strtof(AttributeOf(o, "Height").c_str(), nullptr);
    Prism(v, o.points, height, true, colour);
    if (!NearCamera(v, o.points.front().data())) return;
    std::string text = o.name + (IsPortal(o) ? " (portal)" : " (vis area)");
    for (const char* key : {"AmbientColor", "AffectedBySun", "SkyOnly", "TerrainVisible", "WaterVisible"}) {
        std::string value = AttributeOf(o, key);
        if (!value.empty()) text += "\n" + std::string(key) + " " + value;
    }
    Label(v, o.points.front().data(), text.c_str(), colour);
}

// a grid line's sample at step s
void SamplePoint(const Grid& g, size_t line, int s, float out[3]) {
    float along = float(s) * kGridStep;
    float across = float(line % size_t(g.side + 1)) * kSectorMetres;
    bool x_line = line <= size_t(g.side);
    out[0] = x_line ? across : along;
    out[1] = x_line ? along : across;
    out[2] = g.heights[line][size_t(s)];
}

void DrawGrid(const View& v) {
    std::shared_ptr<const Grid> grid = CurrentGrid();
    const Grid& g = *grid;
    for (size_t line = 0; line < g.heights.size(); line++) {
        int steps = int(g.heights[line].size());
        for (int s = 1; s < steps; s++) {
            float a[3];
            float b[3];
            SamplePoint(g, line, s - 1, a);
            SamplePoint(g, line, s, b);
            Line(v, a, b, kGridColour);
        }
    }
    // each sector named on its west edge, halfway along
    for (int row = 0; row < g.side && !g.heights.empty(); row++) {
        for (int column = 0; column < g.side; column++) {
            float at[3];
            SamplePoint(g, size_t(column), row * kStepsPerSector + kStepsPerSector / 2, at);
            if (!NearCamera(v, at)) continue;
            char text[32];
            std::snprintf(text, sizeof(text), "Sector %d", row * g.side + column);
            Label(v, at, text, kGridColour);
        }
    }
}

void DrawViewDist(const View& v, const ViewEntity& e) {
    DrawEntityAreas(v, e.name, e.pos, kViewDistColour);
    Square(v, e.pos, kViewDistColour);
    if (!NearCamera(v, e.pos)) return;
    std::string text = e.name + " (ViewDist)\nMaxViewDist " + PropertyOf(e, "MaxViewDist");
    Label(v, e.pos, text.c_str(), kViewDistColour);
}

}  // namespace

namespace fcip::editor {

// one line of the grid a frame; the view gets it once a whole pass is done
void UpdateSectorGrid(PPCContext& ctx) {
    int side = int(R32(SYM_Terrain_SectorsPerSide));
    if (side <= 0) return;
    size_t lines = size_t(side + 1) * 2;
    if (g_building.side != side) {
        g_building = Grid{side, std::vector<std::vector<float>>(lines)};
        g_next_line = 0;
    }
    int steps = side * kStepsPerSector + 1;
    std::vector<float>& heights = g_building.heights[g_next_line];
    heights.resize(size_t(steps));
    for (int s = 0; s < steps; s++) {
        float along = float(s) * kGridStep;
        float across = float(g_next_line % size_t(side + 1)) * kSectorMetres;
        bool x_line = g_next_line <= size_t(side);
        heights[size_t(s)] = TerrainHeightAt(ctx, x_line ? across : along, x_line ? along : across);
    }
    if (++g_next_line < lines) return;
    g_next_line = 0;
    auto done = std::make_shared<Grid>(g_building);
    std::lock_guard lock(g_lock);
    g_grid = std::move(done);
}

void ForgetSectorGrid() {
    g_building = Grid{};
    g_next_line = 0;
    std::lock_guard lock(g_lock);
    g_grid = std::make_shared<Grid>();
}

void DrawCulling(const View& v) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    for (const AiObject& o : ai->objects) {
        if (o.type == "VisArea" || o.type == "Portal") DrawVisArea(v, o);
    }
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.cls == "ViewDist") DrawViewDist(v, e);
    }
    DrawGrid(v);
}

}  // namespace fcip::editor
