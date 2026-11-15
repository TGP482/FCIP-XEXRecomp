// the frame's shape, and the game's 2D scaled across so each layout keeps the shape it was made for
#pragma once

#include <cstdint>

namespace fcip {

float DisplayAspect();  // the output's shape, or the map editor's view, 4:3 at the narrowest: the frame is shown that wide
bool DisplayWide();     // the game's 16:9 mode, else its 4:3 one; fixed once the device is up
float Scale2D();        // the x scale of the game's 2D: below 1 on a frame wider than the layout drawing, above narrower
void Anchor2D(float side);  // where scaled 2D sits on this thread: 0 left, 0.5 centre, 1 right
float WidgetAnchor(uint32_t widget);  // the anchor a UI widget drew at last frame, 0.5 before it has drawn
void AspectTick();          // each rendered frame: a new output shape into the renderer

struct Box {
    float x, y, w, h;
};
// any thread: the map editor's 3D view in client pixels, a zero size box for the whole window
void SetViewBox(const Box& view);
// any thread: where the frame sits in a client area of w by h, in the editor's view if set, boxed to DisplayAspect
Box FrameBox(float w, float h);

}  // namespace fcip
