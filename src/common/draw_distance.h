// draw distance multipliers on the level's own values
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip {

// the level's own sun shadow radius and detail distance, from its XML and Game:SetDynamicShadowRadius
void NoteLevelRadius(float radius);
void NoteLevelDetail(float detail);

// instruction hook: a register loaded with engine+0x1174 by a sun shadow radius reader
void ScaleShadowRadius(double& radius);

}  // namespace fcip
