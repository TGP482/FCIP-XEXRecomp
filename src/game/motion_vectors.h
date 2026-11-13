// Motion vectors for FSR from the camera's pass
#pragma once
#include <cstdint>

namespace fcip {

// the scene's draw list: before it is drawn (r3 the constant block), before each item is popped, after the last
void MotionSceneDraw(uint32_t block);
void MotionScenePop(uint32_t list);
void MotionSceneDone();

}  // namespace fcip
