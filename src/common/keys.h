// keyboard and mouse: key state from the window, the binds, and keyboard and mouse against the pad
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace fcip::keys {

// the game's actions on its classic pad layout (input script and controller help), in panel order
enum Action : int {
    kMoveForward, kMoveBack, kStrafeLeft, kStrafeRight, kWalk, kJump, kCrouch, kFire, kAim, kMelee, kReload,
    kInteract, kFeralRun, kFeralVision, kThrow, kGrenades, kWeapons, kBinoculars, kPistol, kRifle, kHeavy,
    kNextWeapon, kPreviousWeapon, kObjectives, kToggleHud, kPause, kSettings, kNoclip, kLookUp, kLookDown, kLookLeft,
    kLookRight, kActionCount
};

const char* Label(int action);
// a key and its alternate, 0 for none
uint8_t Key(int action, int slot);
void Bind(int action, uint8_t vk, int slot);  // 0 clears
void ResetBinds();
// the wheel's notches as keys, on virtual key codes Windows leaves unassigned
constexpr uint8_t kWheelUp = 0x0E;
constexpr uint8_t kWheelDown = 0x0F;

// Binds name the US layout key at a position: a key goes by where it sits, whatever the layout prints on it
std::string KeyName(uint8_t vk);   // as the settings file names it, "" for none
std::string KeyLabel(uint8_t vk);  // as the player's keyboard prints it, for the panel and the prompts
uint8_t PositionKey(uintptr_t vk);  // a key the window was sent, as the key binds name at its position

// guest threads: the key is held and was not started while the pad was in use or the panel was open; a key
// pressed since the last poll counts only when fresh is set
bool Held(int action, bool fresh = false);
bool KeyHeld(uint8_t vk);  // a key itself, bound or not

// window thread: every message after the panel's; true takes it
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
void WindowUp(void* hwnd);  // raw mouse

// Delete went down in the last 250 ms, once per press
bool TakeDelete();

// the next key or mouse button pressed goes to Captured() instead of the game; Escape cancels
void BeginCapture();
bool Capturing();
// once: the key, -1 cancelled, 0 still waiting
int Captured();

// the pad was off neutral in the last 150 ms; keyboard or mouse was used after the pad
bool PadRecent();
bool KbmActive();
void NoteKbm();

// once per pad poll on port 0: tracks the keys against the pad
void TrackKeys(bool pad_moved, bool menu_open);
// raw mouse motion since the last call
void TakeMouse(double& dx, double& dy);

// settings file
void Load(const void* table);
void Save(void* table);

}  // namespace fcip::keys
