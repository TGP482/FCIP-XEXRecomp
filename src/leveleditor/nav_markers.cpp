// ige.xex, the solo level editor: the AI navigation graph the level loaded. The AI system reads GraphManager.bai and
// each Graph%d.bai from the level's folder (8228CF00, CGraph::Read 822C2148, as default.xex 822E4A00 and 822DEDE0):
// one graph per EnclosingArea, in the order of their names, and one per waypoint building. A graph holds its
// boundary, the walkable polygons with their neighbours, the obstacles cut out of them, its exits to other graphs
// and the hide points. The view draws them as the game loaded them, within reach of the camera, and on its own toggle
// the graphs a custom level's save would write now, built again whenever the AI objects change.
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kernel/kernel.h"
#include "leveleditor/markers.h"
#include "leveleditor/nav_graph.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// the graph manager: the graphs' pointers in a vector at +0x24
constexpr uint32_t kManagerGraphsBegin = 0x28;
constexpr uint32_t kManagerGraphsEnd = 0x2C;
// CGraph (0x130 bytes); vectors keep begin at +4 and end at +8 of where they sit
constexpr uint32_t kGraphVerts = 0x20;       // Vec3 each
constexpr uint32_t kGraphBoundary = 0x30;    // vertex indices, u32 each
constexpr uint32_t kGraphExits = 0x90;       // 0x24 each
constexpr uint32_t kGraphHidePoints = 0xAC;  // 0x24 each
constexpr uint32_t kGraphObstacles = 0x40;   // pool: elements at +4, u16 count at +0x14
constexpr uint32_t kGraphPolygons = 0x60;    // pool of 0x3C byte polygons
constexpr uint32_t kPoolElements = 0x04;
constexpr uint32_t kPoolCount = 0x14;
constexpr uint32_t kObstacleBytes = 0x20;
constexpr uint32_t kObstacleIndices = 0x04;
constexpr uint32_t kPolygonBytes = 0x3C;
constexpr uint32_t kPolygonLinks = 0x04;     // pointers to neighbour polygons
constexpr uint32_t kPolygonIndices = 0x14;
constexpr uint32_t kPolygonCentre = 0x24;
constexpr uint32_t kPolygonFlags = 0x34;
constexpr uint8_t kPolygonWaypoint = 0x20;  // a waypoint building's node, no polygon (sets graph +0xE8)
constexpr uint32_t kExitBytes = 0x24;
constexpr uint32_t kExitPoints = 0x04;
constexpr uint32_t kHidePointBytes = 0x24;
constexpr uint32_t kHidePointPos = 0x14;
constexpr uint32_t kHidePointType = 0x20;
constexpr uint32_t kVecBegin = 0x04;
constexpr uint32_t kVecEnd = 0x08;
constexpr uint32_t kMaxItems = 0x40000;  // a guard on each walk

constexpr float kDrawnWithin = 250.0f;  // metres from the camera
constexpr ImU32 kBoundaryColour = IM_COL32(120, 255, 200, 255);
constexpr ImU32 kPolygonColour = IM_COL32(120, 255, 200, 70);
constexpr ImU32 kObstacleColour = IM_COL32(255, 80, 80, 200);
constexpr ImU32 kExitColour = IM_COL32(80, 220, 255, 255);
constexpr ImU32 kWaypointColour = IM_COL32(255, 230, 90, 220);
constexpr ImU32 kHideColour = IM_COL32(180, 255, 120, 200);
constexpr ImU32 kRebuiltColour = IM_COL32(255, 170, 60, 110);
constexpr ImU32 kRebuiltEdgeColour = IM_COL32(255, 170, 60, 255);

using Vec = std::array<float, 3>;
struct Polygon {
    std::vector<int> corners;  // into verts
    std::vector<int> links;    // into polygons
    Vec centre{};
    bool waypoint = false;
};
struct HidePoint {
    Vec pos{};
    int type = 0;
};
struct NavGraph {
    std::vector<Vec> verts;
    std::vector<int> boundary;
    std::vector<Polygon> polygons;
    std::vector<std::vector<int>> obstacles;
    std::vector<std::vector<Vec>> exits;
    std::vector<HidePoint> hide_points;
};
using Nav = std::vector<NavGraph>;

std::mutex g_lock;
std::string g_folder;  // editor thread
std::shared_ptr<const Nav> g_nav = std::make_shared<Nav>();
std::shared_ptr<const Nav> g_rebuilt = std::make_shared<Nav>();
uint32_t g_rebuilt_generation = 0;  // editor thread, the AI objects the rebuilt graphs were cut for
bool g_rebuilt_any = false;

Vec ReadVec(uint32_t at) { return {GF(at), GF(at + 4), GF(at + 8)}; }

// a vector of stride byte items at at, item by item
template <typename F>
void ForEachItem(uint32_t at, uint32_t stride, F&& fn) {
    uint32_t begin = R32(at + kVecBegin);
    uint32_t end = R32(at + kVecEnd);
    if (!begin || end < begin || (end - begin) / stride > kMaxItems) return;
    for (uint32_t item = begin; item < end; item += stride) fn(item);
}

std::vector<int> ReadIndices(uint32_t at) {
    std::vector<int> out;
    ForEachItem(at, 4, [&](uint32_t item) { out.push_back(int(R32(item))); });
    return out;
}

NavGraph ReadGraph(uint32_t graph) {
    NavGraph g;
    ForEachItem(graph + kGraphVerts, 12, [&](uint32_t item) { g.verts.push_back(ReadVec(item)); });
    g.boundary = ReadIndices(graph + kGraphBoundary);
    uint32_t polygons = R32(graph + kGraphPolygons + kPoolElements);
    uint32_t polygon_count = R16(graph + kGraphPolygons + kPoolCount);
    for (uint32_t i = 0; polygons && i < polygon_count; i++) {
        uint32_t p = polygons + i * kPolygonBytes;
        Polygon poly;
        poly.corners = ReadIndices(p + kPolygonIndices);
        poly.centre = ReadVec(p + kPolygonCentre);
        poly.waypoint = (R8(p + kPolygonFlags) & kPolygonWaypoint) != 0;
        ForEachItem(p + kPolygonLinks, 4, [&](uint32_t item) {
            poly.links.push_back(int((R32(item) - polygons) / kPolygonBytes));
        });
        g.polygons.push_back(std::move(poly));
    }
    uint32_t obstacles = R32(graph + kGraphObstacles + kPoolElements);
    uint32_t obstacle_count = R16(graph + kGraphObstacles + kPoolCount);
    for (uint32_t i = 0; obstacles && i < obstacle_count; i++) {
        g.obstacles.push_back(ReadIndices(obstacles + i * kObstacleBytes + kObstacleIndices));
    }
    ForEachItem(graph + kGraphExits, kExitBytes, [&](uint32_t exit) {
        std::vector<Vec> points;
        ForEachItem(exit + kExitPoints, 12, [&](uint32_t item) { points.push_back(ReadVec(item)); });
        g.exits.push_back(std::move(points));
    });
    ForEachItem(graph + kGraphHidePoints, kHidePointBytes, [&](uint32_t item) {
        g.hide_points.push_back({ReadVec(item + kHidePointPos), int(R8(item + kHidePointType))});
    });
    return g;
}

void KeepNav(uint32_t manager) {
    auto nav = std::make_shared<Nav>();
    uint32_t begin = R32(manager + kManagerGraphsBegin);
    uint32_t end = R32(manager + kManagerGraphsEnd);
    for (uint32_t at = begin; begin && at < end && nav->size() < kMaxItems; at += 4) {
        if (uint32_t graph = R32(at)) nav->push_back(ReadGraph(graph));
    }
    std::lock_guard lock(g_lock);
    g_nav = std::move(nav);
}

std::shared_ptr<const Nav> CurrentNav() {
    std::lock_guard lock(g_lock);
    return g_nav;
}

std::shared_ptr<const Nav> RebuiltNav() {
    std::lock_guard lock(g_lock);
    return g_rebuilt;
}

std::vector<int> Indices(const std::vector<uint32_t>& from) { return {from.begin(), from.end()}; }

// a rebuilt graph file as the view draws it; a polygon's first corner stands for its place
NavGraph GraphOf(const NavShape& s) {
    NavGraph g;
    g.verts = s.verts;
    g.boundary = Indices(s.boundary);
    for (const std::vector<uint32_t>& o : s.obstacles) g.obstacles.push_back(Indices(o));
    for (const std::vector<uint32_t>& corners : s.polygons) {
        Polygon p;
        p.corners = Indices(corners);
        if (!corners.empty() && corners.front() < s.verts.size()) p.centre = s.verts[corners.front()];
        g.polygons.push_back(std::move(p));
    }
    for (const NavPoint& node : s.nodes) g.polygons.push_back({{}, {}, node, true});
    return g;
}

bool InReach(const View& v, const Vec& p) {
    float dx = p[0] - v.eye[0];
    float dy = p[1] - v.eye[1];
    return v.has_eye && dx * dx + dy * dy < kDrawnWithin * kDrawnWithin;
}

void Loop(const View& v, const NavGraph& g, const std::vector<int>& corners, ImU32 colour) {
    size_t n = corners.size();
    for (size_t i = 0; i < n; i++) {
        int a = corners[i];
        int b = corners[(i + 1) % n];
        if (a < 0 || b < 0 || size_t(a) >= g.verts.size() || size_t(b) >= g.verts.size()) continue;
        const Vec& from = g.verts[size_t(a)];
        if (InReach(v, from)) Line(v, from.data(), g.verts[size_t(b)].data(), colour);
    }
}

void DrawRebuiltGraph(const View& v, const NavGraph& g) {
    Loop(v, g, g.boundary, kRebuiltEdgeColour);
    for (const std::vector<int>& o : g.obstacles) Loop(v, g, o, kRebuiltEdgeColour);
    for (const Polygon& p : g.polygons) {
        if (!InReach(v, p.centre)) continue;
        if (p.waypoint) Square(v, p.centre.data(), kRebuiltEdgeColour);
        else Loop(v, g, p.corners, kRebuiltColour);
    }
}

void DrawGraph(const View& v, const NavGraph& g) {
    Loop(v, g, g.boundary, kBoundaryColour);
    for (const std::vector<int>& o : g.obstacles) Loop(v, g, o, kObstacleColour);
    for (const Polygon& p : g.polygons) {
        if (!InReach(v, p.centre)) continue;
        if (!p.waypoint) {
            Loop(v, g, p.corners, kPolygonColour);
            continue;
        }
        Square(v, p.centre.data(), kWaypointColour);
        for (int link : p.links) {
            if (link >= 0 && size_t(link) < g.polygons.size()) {
                Line(v, p.centre.data(), g.polygons[size_t(link)].centre.data(), kWaypointColour);
            }
        }
    }
    for (const std::vector<Vec>& exit : g.exits) {
        for (size_t i = 1; i < exit.size(); i++) Line(v, exit[i - 1].data(), exit[i].data(), kExitColour);
    }
    for (const HidePoint& h : g.hide_points) {
        if (InReach(v, h.pos)) Circle(v, h.pos.data(), 0.3f, kHideColour);
    }
}

}  // namespace

std::string fcip::editor::LoadedNavFolder() { return g_folder; }

void fcip::editor::DrawNav(const View& v) {
    for (const NavGraph& g : *CurrentNav()) DrawGraph(v, g);
}

void fcip::editor::DrawNavRebuilt(const View& v) {
    for (const NavGraph& g : *RebuiltNav()) DrawRebuiltGraph(v, g);
}

void fcip::editor::UpdateNavRebuild(PPCContext& ctx) {
    std::shared_ptr<const AiObjects> objects = ReadAiObjects();
    bool current = g_rebuilt_any && objects->generation == g_rebuilt_generation;
    if (!g_view_shown[kViewNavRebuilt] || !objects->kept || current) return;
    auto nav = std::make_shared<Nav>();
    for (const NavFile& file : RebuildNavGraphs(ctx)) nav->push_back(GraphOf(ShapeOfGraph(file)));
    g_rebuilt_generation = objects->generation;
    g_rebuilt_any = true;
    std::lock_guard lock(g_lock);
    g_rebuilt = std::move(nav);
}

PPC_EXTERN_FUNC(__imp__sub_8228CF00);

// the AI system loads the level's navigation graphs (r3 the graph manager, r4 the level's folder)
PPC_FUNC(sub_8228CF00) {
    if (!g_solo) return __imp__sub_8228CF00(ctx, base);
    uint32_t manager = uint32_t(ctx.r[3]);
    g_folder = ReadCString(uint32_t(ctx.r[4]));
    __imp__sub_8228CF00(ctx, base);
    KeepNav(manager);
}
