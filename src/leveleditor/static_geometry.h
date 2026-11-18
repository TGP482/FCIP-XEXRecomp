// the solo level editor's Static Geometry tab: the terrain sectors' brushes shown, picked and placed
// (static_geometry.cpp)
#pragma once

#include <filesystem>
#include <vector>

#include <imgui.h>

#include "leveleditor/view_draw.h"

struct PPCContext;

namespace fcip::editor {

// window thread
void DrawStaticGeometryPanel();
void DrawStaticGeometry(const View& v);        // the picked brush and, when shown, the boxes near the camera
bool BrushMouse(ImVec2 mouse, bool pressed);  // true when picking took the click

void UpdateStaticGeometry(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh read after any

// a brush's world box on the ground
struct BrushFootprint {
    float min_x = 0;
    float min_y = 0;
    float max_x = 0;
    float max_y = 0;
};
std::vector<BrushFootprint> BrushFootprints();  // editor thread, every sector's brushes as they stand, added included
// editor thread, for the navigation: where brushes stood as the level opened that moved or went, and where moved and
// added ones stand now
struct BrushChanges {
    std::vector<BrushFootprint> cleared;
    std::vector<BrushFootprint> placed;
};
BrushChanges ChangedBrushes();
// editor thread, the save: added brushes appended to and removed ones dropped from the folder's sdat\sd%d.sdat files
void WriteBrushEdits(const std::filesystem::path& folder);

}  // namespace fcip::editor
