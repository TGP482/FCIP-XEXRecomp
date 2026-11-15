// the camera's view as its last pass drew it, for what the host draws over the frame
#pragma once

namespace fcip {

// any thread: world space into clip space, row major (a point times it); false before a camera pass was drawn
bool ViewClip(float out[16]);

}  // namespace fcip
