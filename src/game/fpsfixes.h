// frame rate fixes and the game's frame clock
#pragma once

namespace fcip {

// the console's frame, which the game's per call rates are tuned to
inline constexpr double kConsoleFrame = 1.0 / 30.0;

// this frame's seconds as the game's timer set them
double FrameTime();

}  // namespace fcip
