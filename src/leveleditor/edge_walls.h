// the solo level editor's edge walls: the collision walls each terrain sector's ShortRange%d.sdat holds after its
// stream table, read as the level opens, shown and edited in the Static Geometry tab and written back by the save
// (edge_walls.cpp)
#pragma once
#include <filesystem>

struct PPCContext;

namespace fcip::editor {

struct View;

void UpdateEdgeWalls(PPCContext& ctx, bool level_opened);  // editor thread, each frame: queued edits, a fresh read
void DrawEdgeWalls(const View& v);                         // window thread
void DrawEdgeWallsPanel();
void WriteWallEdits(const std::filesystem::path& folder);  // editor thread, the save, once the sector files are written

}  // namespace fcip::editor
