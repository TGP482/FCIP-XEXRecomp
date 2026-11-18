// the AI navigation graphs a custom level's save writes (nav_graph.cpp): Graph%d.bai, one per EnclosingArea in the
// order of their names and then the waypoint buildings' own, and GraphManager.bai, in the format default.xex 822DEDE0
// reads; the view draws them before the save does
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PPCContext;

namespace fcip::editor {

using NavPoint = std::array<float, 3>;
using NavRing = std::vector<NavPoint>;
using NavFile = std::vector<uint8_t>;

struct NavArea {
    std::string name;
    NavRing points;
};

struct NavInput {
    std::vector<NavFile> loaded_graphs;      // as the level loaded them
    std::vector<std::string> loaded_areas;   // the EnclosingAreas the level loaded with
    std::vector<NavRing> loaded_forbidden;   // its ForbiddenAreas
    std::vector<NavArea> areas;              // the EnclosingAreas now
    std::vector<NavRing> forbidden;          // the ForbiddenAreas now
    std::vector<NavRing> cleared;            // where static brushes stood that moved or went; their obstacles drop
};

using GroundHeight = std::function<float(float x, float y)>;

// Graph%d.bai in order
std::vector<NavFile> BuildNavGraphs(const NavInput& in, const GroundHeight& height);
NavFile NavManagerFile(uint32_t graphs);

// a graph file as the view draws it: corners index verts
struct NavShape {
    std::vector<NavPoint> verts;
    std::vector<uint32_t> boundary;
    std::vector<std::vector<uint32_t>> polygons;
    std::vector<std::vector<uint32_t>> obstacles;
    std::vector<NavPoint> nodes;  // a waypoint building's
};
NavShape ShapeOfGraph(const NavFile& file);  // empty when it does not read

// editor thread, custom_save.cpp: the graphs a save would write now
std::vector<NavFile> RebuildNavGraphs(PPCContext& ctx);
// editor thread, each editing frame: the rebuilt graphs for the view while it shows them (nav_markers.cpp)
void UpdateNavRebuild(PPCContext& ctx);

// the 8228CF00 hook keeps where the level's graphs came from (nav_markers.cpp)
std::string LoadedNavFolder();

}  // namespace fcip::editor
