// Keyboard and mouse as the pad, the walk key, the diagonal clamp, crouch while moving, reload and interact as
// separate keys, weapon cycling keys, raw mouse look and no rumble on keyboard and mouse. The hooks run on the
// guest thread, the messages on the window thread.
#include "common/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "common/gyro.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/settings.h"
#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;
using fcip::keys::Held;
namespace K = fcip::keys;

namespace {

// CXEntityProcessingCmd, see CXGame::OnAction and CPlayer::ProcessMovements
constexpr uint32_t kCmdActions = 0x04;  // bit (id - 1) for ids below 32
constexpr uint32_t kCmdAnglePitch = 0x18;  // the view's pitch in degrees, positive looks down
constexpr uint32_t kCmdMoveX = 0x0C;    // MOVE_LR, strafe
constexpr uint32_t kCmdMoveY = 0x10;    // MOVE_FB, forward
constexpr uint32_t kCmdYaw = 0xBC;      // look deltas in degrees, written by CAimHelper::Update
constexpr uint32_t kCmdPitch = 0xC0;
constexpr uint32_t kMoveLeft = 1;
constexpr uint32_t kMoveRight = 2;
constexpr uint32_t kMoveForward = 3;
constexpr uint32_t kMoveBackward = 4;
// RUN, set once an axis passes 0.7: feral run and no crouch while set
constexpr uint32_t kRunBit = 1u << (9 - 1);
constexpr float kWalkSpeed = 0.4f;  // of a full stick, under the 0.7 RUN threshold
// PRIMARY_WEAPON 0x17, SECONDARY_WEAPON 0x18, SPECIAL_WEAPON 0x19
constexpr uint32_t kSlotBits = 0x01C00000u;
constexpr uint32_t kReloadUseBit = 1u << (0x15 - 1);
constexpr uint32_t kZoomBit = 1u << (0x1B - 1);
constexpr uint32_t kSecondaryFireBit = 1u << (0x12 - 1);

// CPlayer
constexpr uint32_t kPlayerInventory = 0xC0;  // 16 byte entries [+0x20, +0x24), id +0, slot +8
constexpr uint32_t kPlayerWeaponObject = 0x2CC;
constexpr uint32_t kPlayerWeapon = 0x2D8;  // current weapon id
constexpr uint32_t kPlayerStance = 0x428;  // 1 stands
constexpr uint32_t kPlayerAutoAim = 0x55C;  // u8, the profile's shooting helper and script SetAutoAimActive
constexpr uint32_t kPlayerLean = 0x7A8;
constexpr uint32_t kPlayerYawDelta = 0x7B8;
constexpr uint32_t kPlayerPitchLag = 0x7BC;  // up to 2 degrees, 4 per second
constexpr uint32_t kPlayerYawLag = 0x7C4;
constexpr uint32_t kPlayerPendingSlot = 0x85C;  // queued slot switch, slot + 1
constexpr uint32_t kPlayerEvolutions = 0x880;  // active modes +0x68: bit 0 feral run, bit 1 the visions
constexpr uint32_t kEvolutionsActive = 0x68;
constexpr uint32_t kPlayerZoom = 0x884;  // zoomed u8 +4, leaving u8 +6
constexpr uint32_t kZoomOn = 0x04;
constexpr uint32_t kZoomLeaving = 0x06;
constexpr uint32_t kPlayerAkimbo = 0x8E0;  // u8, SECONDARY_FIRE fires the second gun
constexpr uint32_t kPlayerCutscene = 0xBA3;  // set by BeginCutScene, cleared by EndCutScene
constexpr uint32_t kPlayerBlurLock = 0x994;  // u8 set stops every blur write (script SetBlur sets it too)
// press trackers read an action: press time float +4 (0 idle), consumed u8 +0xC
constexpr uint32_t kPlayerStanceTracker = 0xBB8;  // MOVEMODE
constexpr uint32_t kPlayerUseTracker = 0xBC0;     // RELOAD_USE
constexpr uint32_t kPlayerFeralTracker = 0xBCC;   // TOGGLE_EVOLUTION, action id at +0
constexpr uint32_t kTrackerTime = 0x04;
constexpr uint32_t kTrackerConsumed = 0x0C;
constexpr uint32_t kInvFirst = 0x20;
constexpr uint32_t kInvLast = 0x24;
constexpr uint32_t kInvEntry = 0x10;
constexpr uint32_t kEntrySlot = 0x08;

// cl_aimingHelper and cl_display_hud: ICVars the game object holds, their value through an int pointer at +0x10
constexpr uint32_t kCVarValuePtr = 0x10;

constexpr uint32_t kMaxClients = 4;  // split screen

// the guest stops calling CAimHelper::Update in menus and on loading screens
constexpr uint64_t kGameplayTimeoutMs = 250;
constexpr UINT_PTR kCursorTimer = 0x46435552;  // "FCUR", no other timer on the window
constexpr UINT kCursorPollMs = 16;
constexpr float kDegreesPerPixel = 0.05f;  // at sensitivity 1
constexpr float kMaxDeltaDegrees = 180.0f;

std::atomic<uint64_t> g_gameplay_ms{0};
// the keyboard's client; CPlayer::ProcessMovements also runs for every AI
std::atomic<uint32_t> g_local_cmd{0};
std::atomic<uint32_t> g_local_player{0};  // set by ProcessView, which only runs for local players
uint32_t g_slot_players[kMaxClients] = {};  // each split screen client's player, guest thread only
// which half of the X button the keys asked for this poll
std::atomic<bool> g_reload_only{false};
std::atomic<bool> g_interact_only{false};
std::atomic<bool> g_pad_y{false};  // the pad's own Y, before the keys merge in
std::atomic<bool> g_pad_aim{false};  // the pad's own right stick click
std::atomic<int64_t> g_start_until{0};  // steady clock ns START stays held to
std::atomic<uint64_t> g_menu_ms{0};  // the last frame the menus took input
std::atomic<bool> g_title{false};
std::atomic<bool> g_scene_hid_hud{false};  // a mission script's HideHud, until its ShowHud

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

bool GuestOk(uint32_t addr) { return addr >= 0x10000u && addr < 0xC0000000u; }

void WF(uint32_t addr, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(addr, raw);
}

bool Local(uint32_t player, uint32_t cmd) { return cmd && cmd == g_local_cmd && GuestOk(player); }

int KeysPort() { return std::max(input::KeyboardPort(), 0); }

// the split screen client the game is updating, with the pad port it polls
struct Client {
    uint32_t slot = 0;
    uint32_t port = 0;
    bool split = false;
};

Client UpdatingClient() {
    Client client;
#if HAS_Game_ClientCount && HAS_Game_Input
    // the slot of the client being updated, 0x30 byte slots holding the port each polls
    constexpr uint32_t kInputSlot = 0xCC;
    constexpr uint32_t kInputSlotSize = 0x30;
    constexpr uint32_t kSlotPort = 0x1C;
    uint32_t game = R32(SYM_Game_pointer);
    uint32_t in = game ? R32(game + SYM_Game_Input) : 0;
    if (!GuestOk(in) || R32(game + SYM_Game_ClientCount) < 2) return client;
    uint32_t slot = R32(in + kInputSlot);
    if (slot >= kMaxClients) return client;
    client = {slot, R32(in + slot * kInputSlotSize + kSlotPort), true};
#endif
    return client;
}

// keyboard and mouse drive the client on their port, or the only one
bool KeysClient(const Client& client) { return !client.split || client.port == uint32_t(KeysPort()); }

// Pad merge

bool Neutral(const input::Pad& p) {
    constexpr int kLeftDead = 7849;
    constexpr int kRightDead = 8689;
    constexpr int kTriggerDead = 30;
    return !p.buttons && p.left_trigger < kTriggerDead && p.right_trigger < kTriggerDead && std::abs(p.lx) < kLeftDead &&
           std::abs(p.ly) < kLeftDead && std::abs(p.rx) < kRightDead && std::abs(p.ry) < kRightDead;
}

int16_t Larger(int16_t a, int32_t b) {
    b = std::clamp(b, -32767, 32767);
    return std::abs(int32_t(a)) >= std::abs(b) ? a : int16_t(b);
}

int32_t Axis(int plus, int minus) { return (Held(plus) ? 32767 : 0) - (Held(minus) ? 32767 : 0); }

struct ButtonBind {
    int action;
    uint16_t mask;
};

constexpr ButtonBind kButtonBinds[] = {
    {K::kBinoculars, input::kDpadUp}, {K::kHeavy, input::kDpadDown}, {K::kRifle, input::kDpadLeft},
    {K::kPistol, input::kDpadRight},  {K::kPause, input::kStart},     {K::kObjectives, input::kBack},
    {K::kCrouch, input::kLeftThumb},  {K::kAim, input::kRightThumb},  {K::kGrenades, input::kLeftShoulder},
    {K::kWeapons, input::kRightShoulder}, {K::kJump, input::kA},      {K::kMelee, input::kB},
    {K::kReload, input::kX},          {K::kFeralRun, input::kY},
};

// the pause key backs out of a menu as B; it keeps the role it went down with, so closing a menu never reopens it
bool PauseBacks(bool menu) {
    static bool back = false;
    bool held = Held(K::kPause, true);
    if (!held) back = menu;
    return held && back;
}

// in the game's menus Enter is A, Backspace B, the move and look keys the d-pad; the other binds act as in play
input::Pad MenuKeys(input::Pad kb) {
    constexpr int kUp[] = {K::kMoveForward, K::kLookUp};
    constexpr int kDown[] = {K::kMoveBack, K::kLookDown};
    constexpr int kLeft[] = {K::kStrafeLeft, K::kLookLeft};
    constexpr int kRight[] = {K::kStrafeRight, K::kLookRight};
    auto any = [](const int (&actions)[2]) { return Held(actions[0]) || Held(actions[1]); };
    kb.lx = kb.ly = kb.rx = kb.ry = 0;
    if (K::KeyHeld(VK_RETURN)) kb.buttons |= input::kA;
    if (K::KeyHeld(VK_BACK)) kb.buttons |= input::kB;
    if (any(kUp)) kb.buttons |= input::kDpadUp;
    if (any(kDown)) kb.buttons |= input::kDpadDown;
    if (any(kLeft)) kb.buttons |= input::kDpadLeft;
    if (any(kRight)) kb.buttons |= input::kDpadRight;
    return kb;
}

input::Pad Keyboard(const input::Pad& pad) {
    input::Pad kb;
    if (K::TextEntry()) return kb;
    for (const ButtonBind& b : kButtonBinds)
        if (Held(b.action)) kb.buttons |= b.mask;
    bool reload = kb.buttons & input::kX;
    bool interact = Held(K::kInteract);
    if (interact) kb.buttons |= input::kX;
    bool pad_x = pad.buttons & input::kX;
    g_reload_only = reload && !interact && !pad_x;
    g_interact_only = interact && !reload && !pad_x;
    kb.left_trigger = Held(K::kThrow) ? 0xFF : 0;
    kb.right_trigger = Held(K::kFire) ? 0xFF : 0;
    kb.lx = int16_t(Axis(K::kStrafeRight, K::kStrafeLeft));
    kb.ly = int16_t(Axis(K::kMoveForward, K::kMoveBack));
    kb.rx = int16_t(Axis(K::kLookRight, K::kLookLeft));
    kb.ry = int16_t(Axis(K::kLookUp, K::kLookDown));
    bool menu = fcip::InMenu();
    if (PauseBacks(menu)) {
        kb.buttons &= ~input::kStart;
        if (menu) kb.buttons |= input::kB;
    } else if (Held(K::kPause, true)) {
        // the menu scan takes START only with no other key held and both sticks under 0.5, a stick latches until
        // it reads 0; so from the moment the pause key goes down it is the only key, and it lands a poll later
        return {.buttons = uint16_t(kb.buttons & input::kStart)};
    }
    return menu ? MenuKeys(kb) : kb;
}

// the editor's view: the movement keys as its camera's stick, though its hidden console page counts as a menu
input::Pad EditorKeys() {
    input::Pad kb;
    if (K::TextEntry()) return kb;
    kb.lx = int16_t(Axis(K::kStrafeRight, K::kStrafeLeft));
    kb.ly = int16_t(Axis(K::kMoveForward, K::kMoveBack));
    return kb;
}

// Gameplay

// cl_aimingHelper's value slot, zeroed for one call while the keyboard or mouse is in use
struct HelperOverride {
    uint32_t slot = 0;
    uint32_t saved = 0;
};

HelperOverride SuppressAimingHelper() {
    if (!K::KbmActive()) return {};
    uint32_t game = R32(SYM_Game_pointer);
    uint32_t cvar = game ? R32(game + SYM_Game_AimingHelper) : 0;
    uint32_t slot = cvar ? R32(cvar + kCVarValuePtr) : 0;
    uint32_t saved = slot ? R32(slot) : 0;
    if (!saved) return {};
    W32(slot, 0);
    return {slot, saved};
}

// the auto aim flag, cleared for one call while the keyboard or mouse is in use
struct AutoAimOverride {
    uint32_t player = 0;
    uint8_t saved = 0;
};

AutoAimOverride SuppressAutoAim(uint32_t player) {
    if (!K::KbmActive() || !GuestOk(player)) return {};
    AutoAimOverride over{player, R8(player + kPlayerAutoAim)};
    W8(player + kPlayerAutoAim, 0);
    return over;
}

void RestoreAutoAim(const AutoAimOverride& over) {
    if (over.player) W8(over.player + kPlayerAutoAim, over.saved);
}

// the game takes no look or aim input in cutscenes, so nothing written straight into the command may either
bool InCutscene(uint32_t player) { return GuestOk(player) && R8(player + kPlayerCutscene); }

// cl_display_hud flips on each press of the toggle key, but not in cutscenes or while a scene hid the HUD
void ToggleHud() {
    static bool was_held = false;
    bool held = Held(K::kToggleHud, true);
    bool pressed = held && !was_held;
    was_held = held;
    if (!pressed) return;
    uint32_t game = R32(SYM_Game_pointer);
    uint32_t cvar = game ? R32(game + SYM_Game_DisplayHud) : 0;
    uint32_t slot = cvar ? R32(cvar + kCVarValuePtr) : 0;
    if (!slot) return;
    uint32_t shown = R32(slot);
    if (shown) g_scene_hid_hud = false;
    if (g_scene_hid_hud || InCutscene(g_local_player)) return;
    W32(slot, shown ? 0 : 1);
}

// looking with the mouse
bool RawLook() { return fcip::MouseLookActive() && K::KbmActive(); }

// aiming down the sights or a scope
bool Zoomed(uint32_t player) {
    uint32_t zoom = GuestOk(player) ? R32(player + kPlayerZoom) : 0;
    return GuestOk(zoom) && R8(zoom + kZoomOn);
}

void MouseLook(uint32_t cmd) {
    uint64_t now = NowMs();
    uint64_t prev = g_gameplay_ms.exchange(now);
    // the editor's view takes the raw motion itself while it is up
    if (fcip::g_editor_shell) return;
    // drained every frame: motion banked while the look was off is stale and snaps the view when applied late
    double dx = 0;
    double dy = 0;
    K::TakeMouse(dx, dy);
    bool running = prev && now - prev <= kGameplayTimeoutMs;
    if (!cmd || !running || !fcip::MouseLookActive() || (dx == 0 && dy == 0)) return;
    if (InCutscene(g_local_player)) return;
    // Steam turns the pad's stick into mouse motion, the pad already looks
    if (K::PadRecent()) return;
    K::NoteKbm();
    const fcip::Settings& s = fcip::Get();
    bool aiming = Zoomed(g_local_player);
    float kx = kDegreesPerPixel * (aiming ? s.aim_sensitivity_x : s.sensitivity_x);
    float ky = kDegreesPerPixel * (aiming ? s.aim_sensitivity_y : s.sensitivity_y);
    float yaw = std::clamp(float((s.invert_x ? -dx : dx) * kx), -kMaxDeltaDegrees, kMaxDeltaDegrees);
    float pitch = std::clamp(float((s.invert_y ? -dy : dy) * ky), -kMaxDeltaDegrees, kMaxDeltaDegrees);
    WF(cmd + kCmdYaw, GF(cmd + kCmdYaw) + yaw);
    WF(cmd + kCmdPitch, GF(cmd + kCmdPitch) + pitch);
}

// the controller's gyro turns the view beside the right stick, its recenter button levels it; off in split screen,
// where its pad's port is unknown
void GyroLook(uint32_t cmd, bool split) {
    const fcip::Settings& s = fcip::Get();
    gyro::Turn turn = gyro::Take(s.gyro);
    if (!cmd || split || fcip::MenuOpen() || InCutscene(g_local_player)) return;
    if (turn.recenter) WF(cmd + kCmdPitch, -std::remainder(GF(cmd + kCmdAnglePitch), 360.0f));
    if (turn.yaw == 0.0f && turn.pitch == 0.0f) return;
    // the gyro turns left positive, the command right positive; both pitch down positive
    float yaw = s.pad_invert_x ? turn.yaw : -turn.yaw;
    WF(cmd + kCmdYaw, GF(cmd + kCmdYaw) + yaw);
    WF(cmd + kCmdPitch, GF(cmd + kCmdPitch) + turn.pitch);
}

bool StandingCrouchPress(uint32_t player) {
    if (!K::KbmActive() || !GuestOk(player) || R32(player + kPlayerStance) != 1) return false;
    uint32_t tracker = R32(player + kPlayerStanceTracker);
    return GuestOk(tracker) && GF(tracker + kTrackerTime) != 0.0f && !R8(tracker + kTrackerConsumed);
}

// The analog branch of ProcessMovements takes |x| and |y| as they are, signed by the move action bits, so a keyboard
// diagonal arrives as 1.41. Only the all zero digital branch normalizes, so walking fills the analog fields.
void OnMovement(uint32_t player, uint32_t cmd) {
    if (cmd != g_local_cmd) return;
    float x = GF(cmd + kCmdMoveX);
    float y = GF(cmd + kCmdMoveY);
    bool walk = !fcip::MenuOpen() && Held(K::kWalk);
    uint32_t actions = R32(cmd + kCmdActions);
    // a keyboard move is full stick, so RUN would block the crouch tap while standing; cleared from the press until
    // the tracker consumes it
    if (walk || StandingCrouchPress(player)) W32(cmd + kCmdActions, actions & ~kRunBit);
    if (x == 0.0f && y == 0.0f) {
        if (!walk) return;
        auto has = [actions](uint32_t id) { return (actions >> (id - 1)) & 1u ? 1.0f : 0.0f; };
        x = has(kMoveRight) - has(kMoveLeft);
        y = has(kMoveForward) - has(kMoveBackward);
    }
    float length = std::sqrt(x * x + y * y);
    if (!(length > 0.0f)) return;
    float scale = length > 1.0f ? 1.0f / length : 1.0f;
    if (walk) scale *= kWalkSpeed;
    if (scale == 1.0f) return;
    WF(cmd + kCmdMoveX, x * scale);
    WF(cmd + kCmdMoveY, y * scale);
}

// akimbo on keyboard and mouse: the aim key is the second trigger
bool AkimboKeys(uint32_t player) {
    return K::KbmActive() && !K::PadRecent() && R8(player + kPlayerAkimbo) && R32(player + kPlayerWeaponObject);
}

// the slot of the weapon in hand, or the one a switch is heading to; 0 when unknown
uint32_t CurrentSlot(uint32_t player) {
    uint32_t pending = R32(player + kPlayerPendingSlot);
    if (pending >= 2 && pending <= 4) return pending - 1;
    uint32_t weapon = R32(player + kPlayerWeapon);
    uint32_t inv = R32(player + kPlayerInventory);
    if (!GuestOk(inv)) return 0;
    uint32_t first = R32(inv + kInvFirst);
    uint32_t last = R32(inv + kInvLast);
    if (!GuestOk(first) || last < first || (last - first) / kInvEntry > 32) return 0;
    for (uint32_t e = first; e < last; e += kInvEntry)
        if (R32(e) == weapon) return R32(e + kEntrySlot);
    return 0;
}

// avail[slot - 1] is the game's own "can switch to" answer, so akimbo never triggers
void CycleSlot(uint32_t player, uint32_t cmd, uint32_t avail, int steps) {
    if (R32(cmd + kCmdActions) & kSlotBits) return;
    static constexpr uint32_t kOrder[] = {2, 1, 3};  // pistol, rifle, heavy: the order of keys 2, 3 and 4
    int dir = steps > 0 ? -1 : 1;  // positive steps go back
    uint32_t current = CurrentSlot(player);
    int at = dir > 0 ? -1 : 3;
    for (int i = 0; i < 3; i++)
        if (kOrder[i] == current) at = i;
    for (int n = 1; n <= 3; n++) {
        uint32_t slot = kOrder[((at + dir * n) % 3 + 3) % 3];
        if (!R8(avail + slot - 1)) continue;
        W32(cmd + kCmdActions, R32(cmd + kCmdActions) | 1u << (0x16 + slot - 1));
        return;
    }
}

// hold to aim sets ZOOM_TOGGLE for one frame only when the held key or stick and the zoom disagree; if the game drops
// the zoom itself (reload) it must be pressed again
void HoldToAim(uint32_t player, uint32_t cmd, bool held) {
    static bool was_in = false;
    static bool latched = false;
    static bool exiting = false;
    static uint64_t retry_ms = 0;
    uint32_t zoom = R32(player + kPlayerZoom);
    bool in = GuestOk(zoom) && R8(zoom + kZoomOn);
    bool leaving = in && R8(zoom + kZoomLeaving);
    if (was_in && !in && held && !exiting) latched = true;
    if (!held) latched = false;
    if (!in) exiting = false;
    uint32_t actions = R32(cmd + kCmdActions) & ~kZoomBit;
    uint64_t now = NowMs();
    // the retry gap stops a toggle the game refuses (no scope, reloading) from clicking every frame
    if (!leaving && (held && !latched) != in && now >= retry_ms) {
        actions |= kZoomBit;
        retry_ms = now + 250;
        exiting = in;
    }
    W32(cmd + kCmdActions, actions);
    was_in = in;
}

// a tracker handed a press: tap for a positive time, a finished hold for a negative one
void Press(uint32_t tracker, float time) {
    WF(tracker + kTrackerTime, time);
    W8(tracker + kTrackerConsumed, 0);
}

}  // namespace

namespace fcip {

uint32_t LocalPlayer() { return g_local_player; }

bool MouseLookActive() {
    if (MenuOpen() || g_editor_shell || GetForegroundWindow() != static_cast<HWND>(app::Window())) return false;
    uint64_t stamp = g_gameplay_ms;
    return stamp && NowMs() - stamp <= kGameplayTimeoutMs;
}

void NoteMenu(bool menu, bool title) {
    g_menu_ms = menu ? NowMs() : 0;
    g_title = title;
}

// play keeps its keys through the moment a menu shows over it, and from when the menus stop updating
bool InMenu() {
    uint64_t stamp = g_menu_ms;
    return stamp && NowMs() - stamp <= kGameplayTimeoutMs && !MouseLookActive();
}
bool OnTitle() { return g_title; }

void PressStart() {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    g_start_until = until.time_since_epoch().count();
}

void GamePad(int port, input::Pad& pad) {
    int keyboard = input::KeyboardPort();
    // the keyboard's port, or player 1's when no player has it
    if (port != KeysPort()) return;
    bool start = std::chrono::steady_clock::now().time_since_epoch().count() < g_start_until;
    K::TrackKeys(pad.connected && !Neutral(pad), MenuOpen());
    g_pad_y = pad.buttons & input::kY;
    g_pad_aim = pad.buttons & input::kRightThumb;
    if (MenuOpen()) {
        g_reload_only = g_interact_only = false;
        pad = {.connected = pad.connected || port == keyboard};
        return;
    }
    ToggleHud();
    input::Pad kb;
    if (port == keyboard) kb = g_editor_view_keys ? EditorKeys() : Keyboard(pad);
    // not plain negation: -32768 has no positive
    if (fcip::Get().pad_invert_x && !InMenu()) pad.rx = int16_t(-1 - pad.rx);
    // keyboard and mouse ride on their port, a controller or not
    if (port == keyboard) pad.connected = true;
    pad.buttons |= kb.buttons;
    pad.left_trigger = std::max(pad.left_trigger, kb.left_trigger);
    pad.right_trigger = std::max(pad.right_trigger, kb.right_trigger);
    pad.lx = Larger(pad.lx, kb.lx);
    pad.ly = Larger(pad.ly, kb.ly);
    pad.rx = Larger(pad.rx, kb.rx);
    pad.ry = Larger(pad.ry, kb.ry);
    if (start) pad.buttons |= input::kStart;
    if (g_noclip_pad) g_noclip_pad(pad);
    if (g_editor_pad) g_editor_pad(pad);
}

void GameRumble(int port, uint16_t& left, uint16_t& right) {
    if (port == KeysPort() && K::KbmActive()) left = right = 0;
    uint32_t strength = fcip::Get().vibration_strength;
    left = uint16_t(std::min<uint32_t>(left * strength / 100, 0xFFFF));
    right = uint16_t(std::min<uint32_t>(right * strength / 100, 0xFFFF));
}

// the cursor the client area shows: none while the mouse looks or a pad is in use; false while the panel keeps its own
static bool WantedCursor(bool locked, HCURSOR& cursor) {
    cursor = nullptr;
    if (locked) return true;
    if (MenuOpen()) return false;
    // the editor's panels want the mouse whatever the pad did
    if (!g_editor_ui && K::PadActive()) return true;
    cursor = static_cast<HCURSOR>(MenuCursor());
    // the window class has none, so once play hid it nothing else brings the arrow back
    if (!cursor) cursor = LoadCursor(nullptr, IDC_ARROW);
    return true;
}

static bool PointerInClient(HWND hwnd) {
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || WindowFromPoint(p) != hwnd || !ScreenToClient(hwnd, &p)) return false;
    return GetClientRect(hwnd, &r) && PtInRect(&r, p);
}

// Mouse look starts and stops with play, not with a mouse message, so a timer swaps the cursor at once.
bool CursorMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    static bool locked = false;
    static bool polling = false;
    static bool pad = false;
    HWND window = static_cast<HWND>(hwnd);
    if (!polling) polling = SetTimer(window, kCursorTimer, kCursorPollMs, nullptr) != 0;
    bool tick = msg == WM_TIMER && wp == kCursorTimer;
    bool look = MouseLookActive();
    bool flipped = look != locked;
    if (flipped && (tick || msg == WM_INPUT || msg == WM_SETCURSOR || msg == WM_MOUSEMOVE)) {
        locked = look;
        RECT r;
        GetClientRect(window, &r);
        POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(window, &centre);
        RECT hold{centre.x, centre.y, centre.x + 1, centre.y + 1};
        ClipCursor(look ? &hold : nullptr);
    }
    if (msg == WM_KILLFOCUS) {
        locked = false;
        ClipCursor(nullptr);
    }
    HCURSOR cursor;
    if (tick) {
        bool active = K::PadActive();
        bool swapped = active != pad;
        pad = active;
        if ((flipped || swapped) && PointerInClient(window) && WantedCursor(locked, cursor)) SetCursor(cursor);
        *result = 0;
        return true;
    }
    if (msg != WM_SETCURSOR || LOWORD(lp) != HTCLIENT || !WantedCursor(locked, cursor)) return false;
    SetCursor(cursor);
    *result = TRUE;
    return true;
}

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ProcessMovements));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_SelectWeaponSlot));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ReloadUse));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Zoom));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Fire));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_FireWeapon));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Feral));
PPC_EXTERN_FUNC(GUEST_ORIG(CAimHelper_AutoAim));
PPC_EXTERN_FUNC(GUEST_ORIG(CAimHelper_Update));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_UpdateLookBones));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_UpdateView));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ProcessView));
PPC_EXTERN_FUNC(GUEST_ORIG(CScriptObjectGame_HideHud));
PPC_EXTERN_FUNC(GUEST_ORIG(CScriptObjectGame_ShowHud));

// (this, cmd): clamps the move vector and applies the walk key
PPC_FUNC(GUEST(CPlayer_ProcessMovements)) {
    if (uint32_t cmd = uint32_t(ctx.r[4])) OnMovement(uint32_t(ctx.r[3]), cmd);
    GUEST_ORIG(CPlayer_ProcessMovements)(ctx, base);
}

// (player, cmd, u8 avail[3]), before ProcessMovements: NEXT and PREVIOUS WEAPON press the next slot the game can
// switch to
PPC_FUNC(GUEST(CPlayer_SelectWeaponSlot)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    uint32_t avail = uint32_t(ctx.r[5]);
    if (cmd && cmd == g_local_cmd) {
        // one slot per press
        static bool was_next = false;
        static bool was_previous = false;
        bool next = Held(K::kNextWeapon);
        bool previous = Held(K::kPreviousWeapon);
        int steps = int(previous && !was_previous) - int(next && !was_next);
        was_next = next;
        was_previous = previous;
        if (steps && !K::PadRecent() && GuestOk(player) && GuestOk(avail)) {
            K::NoteKbm();
            CycleSlot(player, cmd, avail, steps);
        }
    }
    GUEST_ORIG(CPlayer_SelectWeaponSlot)(ctx, base);
}

// (player, cmd, u8 can_reload), the only reader of the X action: a press uses or stows, a hold swaps, an unconsumed
// tap reloads. Interact alone never reloads; reload alone hides the press from the use path and hands the tracker a
// tap, so it reloads at once.
PPC_FUNC(GUEST(CPlayer_ReloadUse)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    bool local = Local(player, cmd);
    bool reload = local && g_reload_only;
    static bool was_reload = false;
    bool edge = reload && !was_reload;
    if (local) was_reload = reload;
    // the reload fires on the tap's release frame, after the interact key is up, so an interact press stays one
    // until the tracker goes idle
    static bool interact = false;
    uint32_t use_tracker = local ? R32(player + kPlayerUseTracker) : 0;
    if (local && g_interact_only) interact = true;
    else if (local && (!GuestOk(use_tracker) || GF(use_tracker + kTrackerTime) == 0.0f)) interact = false;
    if (local && interact) ctx.r[5] = 0;
    uint32_t actions = reload ? R32(cmd + kCmdActions) : 0;
    if (reload) {
        W32(cmd + kCmdActions, actions & ~kReloadUseBit);
        if (edge && (ctx.r[5] & 0xFF) && GuestOk(use_tracker)) Press(use_tracker, 1.0e30f);
    }
    GUEST_ORIG(CPlayer_ReloadUse)(ctx, base);
    if (reload) W32(cmd + kCmdActions, actions);
}

// (player, cmd): toggles the zoom every frame ZOOM_TOGGLE is set
PPC_FUNC(GUEST(CPlayer_Zoom)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    if (Local(player, cmd) && AkimboKeys(player)) {
        // the aim key fires the second gun, a zoom would also drop akimbo
        W32(cmd + kCmdActions, R32(cmd + kCmdActions) & ~kZoomBit);
    } else if (Local(player, cmd) && fcip::Get().hold_aim && K::KbmActive() && !K::PadRecent() && !InCutscene(player)) {
        HoldToAim(player, cmd, Held(K::kAim));
    } else if (Local(player, cmd) && fcip::Get().pad_hold_aim && !K::KbmActive() && !InCutscene(player) &&
               !R8(player + kPlayerAkimbo)) {
        HoldToAim(player, cmd, g_pad_aim);
    }
    // zoom steps set the scope blur; the lock keeps it off
    bool lock = !fcip::Get().blur && GuestOk(player);
    uint8_t saved = lock ? R8(player + kPlayerBlurLock) : 0;
    if (lock) W8(player + kPlayerBlurLock, 1);
    GUEST_ORIG(CPlayer_Zoom)(ctx, base);
    if (lock) W8(player + kPlayerBlurLock, saved);
}

// (player, cmd, u8 can_fire): SECONDARY_FIRE fires the second gun in akimbo and throws otherwise, so it only rides
// the aim key while the guns can fire
PPC_FUNC(GUEST(CPlayer_Fire)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    bool second = Local(player, cmd) && (ctx.r[5] & 0xFF) && AkimboKeys(player) && Held(K::kAim);
    uint32_t actions = second ? R32(cmd + kCmdActions) : 0;
    if (second) W32(cmd + kCmdActions, actions | kSecondaryFireBit);
    GUEST_ORIG(CPlayer_Fire)(ctx, base);
    if (second) W32(cmd + kCmdActions, actions);
}

// (player, cmd): the pad's Y is a tap for feral run and a hold for feral vision. The keys get one each, both on the
// press; hold to sprint taps again on release.
PPC_FUNC(GUEST(CPlayer_Feral)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    uint32_t tracker = Local(player, cmd) ? R32(player + kPlayerFeralTracker) : 0;
    uint32_t id = GuestOk(tracker) ? R32(tracker) : 0;
    uint32_t bit = id - 1 < 32 ? 1u << (id - 1) : 0;
    uint32_t actions = bit ? R32(cmd + kCmdActions) : 0;
    static bool run_was = false;
    static bool vision_was = false;
    if (bit && !g_pad_y) {
        bool keys = !fcip::MenuOpen();
        bool run = keys && Held(K::kFeralRun);
        bool vision = keys && Held(K::kFeralVision);
        uint32_t evolutions = R32(player + kPlayerEvolutions);
        bool running = GuestOk(evolutions) && (R32(evolutions + kEvolutionsActive) & 1);
        bool tap = fcip::Get().hold_sprint ? run != run_was && run != running : run && !run_was;
        bool see = vision && !vision_was;
        uint32_t now = actions & ~bit;
        if (see || tap) Press(tracker, see ? -1.0e30f : 1.0e30f);
        if (see) now |= bit;
        run_was = run;
        vision_was = vision;
        W32(cmd + kCmdActions, now);
    }
    GUEST_ORIG(CPlayer_Feral)(ctx, base);
    if (bit) W32(cmd + kCmdActions, actions);
}

// (helper, player, ...): turns the weapon toward the target; with the flag clear it takes its reset path
PPC_FUNC(GUEST(CAimHelper_AutoAim)) {
    bool keys = KeysClient(UpdatingClient());
    AutoAimOverride over = keys ? SuppressAutoAim(uint32_t(ctx.r[4])) : AutoAimOverride{};
    GUEST_ORIG(CAimHelper_AutoAim)(ctx, base);
    RestoreAutoAim(over);
}

// (this): fires the weapon, which the auto aim flag helps onto the target; local player only
PPC_FUNC(GUEST(CPlayer_FireWeapon)) {
    uint32_t player = uint32_t(ctx.r[3]);
    AutoAimOverride over = player == g_local_player ? SuppressAutoAim(player) : AutoAimOverride{};
    GUEST_ORIG(CPlayer_FireWeapon)(ctx, base);
    RestoreAutoAim(over);
}

// (helper, cmd): mouse look and the gyro add this frame's turn; the aiming helper is off for the call while the
// keyboard or mouse is in use. A split screen client on another port gets its pad alone.
PPC_FUNC(GUEST(CAimHelper_Update)) {
    uint32_t cmd = uint32_t(ctx.r[4]);
    Client client = UpdatingClient();
    if (!KeysClient(client)) {
        GUEST_ORIG(CAimHelper_Update)(ctx, base);
        return;
    }
    g_local_cmd = cmd;
    HelperOverride helper = SuppressAimingHelper();
    GUEST_ORIG(CAimHelper_Update)(ctx, base);
    if (helper.slot) W32(helper.slot, helper.saved);
    MouseLook(cmd);
    GyroLook(cmd, client.split);
}

// (this), local player only: looking with the mouse, a second pass sees no pitch change, so the lag stays zero and
// the lean steps once
PPC_FUNC(GUEST(CPlayer_UpdateLookBones)) {
    uint32_t player = uint32_t(ctx.r[3]);
    bool raw = player && player == g_local_player && RawLook();
    float lean = raw ? GF(player + kPlayerLean) : 0.0f;
    GUEST_ORIG(CPlayer_UpdateLookBones)(ctx, base);
    if (!raw) return;
    WF(player + kPlayerLean, lean);
    WF(player + kPlayerPitchLag, 0.0f);
    ctx.r[3] = player;
    GUEST_ORIG(CPlayer_UpdateLookBones)(ctx, base);
}

// (this): its weapon yaw lag, zeroed like the pitch one. Noclip moves the player first, so the camera and the weapon
// built from its position this frame agree
PPC_FUNC(GUEST(CPlayer_UpdateView)) {
    uint32_t player = uint32_t(ctx.r[3]);
    bool local = player && player == g_local_player;
    uint32_t cmd = g_local_cmd;
    if (local && cmd && fcip::g_noclip_move) fcip::g_noclip_move(ctx, player, cmd);
    if (local && fcip::g_play_tick) fcip::g_play_tick(ctx);
    GUEST_ORIG(CPlayer_UpdateView)(ctx, base);
    if (!local || !RawLook()) return;
    WF(player + kPlayerYawDelta, 0.0f);
    WF(player + kPlayerYawLag, 0.0f);
}

// (this, cmd) applies this frame's look to the entity; the camera, the weapon yaw and the look bones were already built
// from last frame's, so all of them show it next frame, as on the console
PPC_FUNC(GUEST(CPlayer_ProcessView)) {
    uint32_t player = uint32_t(ctx.r[3]);
    Client client = UpdatingClient();
    g_slot_players[client.slot] = player;
    if (KeysClient(client)) g_local_player = player;
    GUEST_ORIG(CPlayer_ProcessView)(ctx, base);
}

// scenes hide the HUD with HideHud and bring it back with ShowHud, so the toggle must not undo them
PPC_FUNC(GUEST(CScriptObjectGame_HideHud)) {
    g_scene_hid_hud = true;
    GUEST_ORIG(CScriptObjectGame_HideHud)(ctx, base);
}

PPC_FUNC(GUEST(CScriptObjectGame_ShowHud)) {
    g_scene_hid_hud = false;
    GUEST_ORIG(CScriptObjectGame_ShowHud)(ctx, base);
}
