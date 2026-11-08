// the frame's shape, and the game's 2D drawn narrower where the frame is wider than the game's own mode
#pragma once

namespace fcip {

float DisplayAspect();  // the output's shape, 4:3 at the narrowest: the frame is shown that wide
bool DisplayWide();     // the game's 16:9 mode, else its 4:3 one; fixed once the device is up
float Narrow2D();       // that mode's shape over the frame's: the x scale of the game's 2D
void Anchor2D(float side);  // where narrowed 2D sits on this thread: 0 left, 0.5 centre, 1 right

}  // namespace fcip
