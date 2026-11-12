// draw distance multipliers on the level's own values
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip {

// the level's own sun shadow radius and detail distance, from its XML and Game:SetDynamicShadowRadius
void NoteLevelRadius(float radius);
void NoteLevelDetail(float detail);

// the grass reach from the camera's sector: the squared gap in sectors when the grass distance reaches this sector
// from anywhere in the camera's, else -1
float GrassReach(int32_t cam_x, int32_t cam_y, int32_t x, int32_t y);

// instruction hooks: a register loaded with engine+0xB30 for the grass tile loop, with engine+0x1174 by a sun
// shadow radius reader
void ScaleGrassRadius(double& radius);
void ScaleShadowRadius(double& radius);

// the sun slope bias of the near map, reach times the stock radius, while it draws, the far map's after
void UseNearSunSlope(bool on, uint32_t reach);

}  // namespace fcip
