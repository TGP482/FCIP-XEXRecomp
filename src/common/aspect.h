// the frame's shape, and the game's 2D scaled across so each layout keeps the shape it was made for
#pragma once

namespace fcip {

float DisplayAspect();  // the output's shape, 4:3 at the narrowest: the frame is shown that wide
bool DisplayWide();     // the game's 16:9 mode, else its 4:3 one; fixed once the device is up
float Scale2D();        // the x scale of the game's 2D: below 1 on a frame wider than the layout drawing, above narrower
void Anchor2D(float side);  // where scaled 2D sits on this thread: 0 left, 0.5 centre, 1 right
void AspectTick();          // each rendered frame: a new output shape into the renderer

}  // namespace fcip
