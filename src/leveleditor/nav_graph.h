// the AI navigation graphs a custom level's save writes (nav_graph.cpp): Graph%d.bai, one per EnclosingArea in the
// order of their names and then the waypoint buildings' own, and GraphManager.bai, in the format default.xex 822DEDE0
// reads
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
};

using GroundHeight = std::function<float(float x, float y)>;

// Graph%d.bai in order
std::vector<NavFile> BuildNavGraphs(const NavInput& in, const GroundHeight& height);
NavFile NavManagerFile(uint32_t graphs);

// the 8228CF00 hook keeps where the level's graphs came from (nav_markers.cpp)
std::string LoadedNavFolder();

}  // namespace fcip::editor
