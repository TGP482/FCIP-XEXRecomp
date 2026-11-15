// the volumetric fog's terrain shade, the heights below which the terrain hides the sun
#pragma once

#include "d3d/occlusion.h"

namespace fcip {

// title thread: the shade around the camera kept current and the view's rows into it set; world is the renderer's view
// space into the world, row major, sea the sea level
void UpdateTerrainShade(const float world[16], float sea, gpu::FogView& view);

}  // namespace fcip
