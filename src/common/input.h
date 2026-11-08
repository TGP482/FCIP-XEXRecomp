// keyboard and mouse as the pad, mouse look and the pointer lock
#pragma once
#include <cstdint>

namespace input {
struct Pad;
}

namespace fcip {

// GameInfo hooks: keys and mouse merge into port 0, no rumble on keyboard and mouse
void GamePad(int port, input::Pad& pad);
void GameRumble(int port, uint16_t& left, uint16_t& right);
// window thread: hides and holds the cursor while the mouse looks
bool CursorMessage(void* hwnd, uint32_t msg, intptr_t lp, intptr_t* result);

// START on port 0 for a moment, as a player presses it
void PressStart();

bool MouseLookActive();
uint32_t LocalPlayer();  // the CPlayer ProcessView last ran for
// the frame's look as the window shows it: 16:9 on a widescreen output, else 4:3
float FrameAspect();

}  // namespace fcip
