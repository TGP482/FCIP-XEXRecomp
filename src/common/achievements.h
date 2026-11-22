// the achievements dialog the main and pause menus' ACHIEVEMENTS open
#pragma once

namespace fcip::achievements {

void Open();
void Close();
bool IsOpen();
void Draw();  // render thread, in the UI frame

}  // namespace fcip::achievements
