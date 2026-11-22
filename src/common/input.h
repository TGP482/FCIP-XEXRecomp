// keyboard and mouse as the pad, mouse look and the pointer lock
#pragma once
#include <cstdint>

struct PPCContext;

namespace input {
struct Pad;
}

namespace fcip {

// GameInfo hooks: keys and mouse merge into port 0, no rumble on keyboard and mouse
void GamePad(int port, input::Pad& pad);
void GameRumble(int port, uint16_t& left, uint16_t& right);
// window thread: hides and holds the cursor while the mouse looks, else shows MenuCursor
bool CursorMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
// HCURSOR from cursor.png or cursor.dds beside the exe sized to the frame, null without one
void* MenuCursor();

// START on port 0 for a moment, as a player presses it
void PressStart();

bool MouseLookActive();
// menu thread, each frame: whether the game's menus take input and PRESS START is up
void NoteMenu(bool menu, bool title);
bool InMenu();
bool OnTitle();
uint32_t LocalPlayer();  // the CPlayer ProcessView last ran for

// default.xex's noclip: port 0 after the keys merged in, then the local player before its view is built
inline void (*g_noclip_pad)(input::Pad& pad) = nullptr;
inline void (*g_noclip_move)(PPCContext& ctx, uint32_t player, uint32_t cmd) = nullptr;
// default.xex's achievement notifications: each frame the local player's view is built
inline void (*g_play_tick)(PPCContext& ctx) = nullptr;
// FCIPEditor.exe's desktop UI: port 0 after the keys and noclip
inline void (*g_editor_pad)(input::Pad& pad) = nullptr;

}  // namespace fcip
