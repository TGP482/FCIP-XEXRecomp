// Keyboard and mouse as the pad, the walk key, the diagonal clamp, crouch while moving, reload and interact as
// separate keys, wheel weapon cycling, raw mouse look and no rumble on keyboard and mouse. The hooks run on the
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
// PRIMARY_WEAPON 0x17, SECONDARY_WEAPON 0x18, SPECIAL_WEAPON 0x19
constexpr uint32_t kSlotBits = 0x01C00000u;
constexpr uint32_t kReloadUseBit = 1u << (0x15 - 1);
constexpr uint32_t kZoomBit = 1u << (0x1B - 1);
constexpr uint32_t kSecondaryFireBit = 1u << (0x12 - 1);

// CPlayer
constexpr uint32_t kPlayerEntity = 0x04;
constexpr uint32_t kPlayerInventory = 0xC0;  // 16 byte entries [+0x20, +0x24), id +0, slot +8
constexpr uint32_t kPlayerWeaponObject = 0x2CC;
constexpr uint32_t kPlayerWeapon = 0x2D8;  // current weapon id
constexpr uint32_t kPlayerStance = 0x428;  // 1 stands
constexpr uint32_t kPlayerAutoAim = 0x55C;  // u8, script SetAutoAimActive, not gated by cl_aimingHelper
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

// the render camera is copied before the player update rebuilds it: angles {pitch, roll, yaw} at entity +0x374,
// camera object at *(entity+0x11C) with angles at +0x14
constexpr uint32_t kEntityAngles = 0x374;
constexpr uint32_t kEntityCamera = 0x11C;
constexpr uint32_t kCameraAngles = 0x14;

// cl_aimingHelper: an ICVar the game object holds, its value through an int pointer at +0x10
constexpr uint32_t kCVarValuePtr = 0x10;

// the guest stops calling CAimHelper::Update in menus and on loading screens
constexpr uint64_t kGameplayTimeoutMs = 250;
constexpr float kDegreesPerPixel = 0.05f;  // at sensitivity 1
constexpr float kMaxDeltaDegrees = 180.0f;

std::atomic<uint64_t> g_gameplay_ms{0};
std::atomic<uint32_t> g_local_cmd{0};     // CPlayer::ProcessMovements also runs for every AI
std::atomic<uint32_t> g_local_player{0};  // set by ProcessView, which only runs for the local player
// which half of the X button the keys asked for this poll
std::atomic<bool> g_reload_only{false};
std::atomic<bool> g_interact_only{false};
std::atomic<bool> g_pad_y{false};  // the pad's own Y, before the keys merge in
std::atomic<int64_t> g_start_until{0};  // steady clock ns START stays held to

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

input::Pad Keyboard(const input::Pad& pad) {
    input::Pad kb;
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
    // the menu scan takes START only with no other key held and both sticks under 0.5, a stick latches until it
    // reads 0; so from the moment the pause key goes down it is the only key, and it lands a poll later
    if (Held(K::kPause, true)) kb = {.buttons = uint16_t(kb.buttons & input::kStart)};
    return kb;
}

// Gameplay

// cl_aimingHelper's value slot, zeroed for one call while the keyboard or mouse is in use
struct HelperOverride {
    uint32_t slot = 0;
    uint32_t saved = 0;
};

HelperOverride SuppressAimingHelper() {
    if (!fcip::Get().auto_helpers || !K::KbmActive()) return {};
    uint32_t game = R32(SYM_Game_pointer);
    uint32_t cvar = game ? R32(game + SYM_Game_AimingHelper) : 0;
    uint32_t slot = cvar ? R32(cvar + kCVarValuePtr) : 0;
    uint32_t saved = slot ? R32(slot) : 0;
    if (!saved) return {};
    W32(slot, 0);
    return {slot, saved};
}

// smoothing off and looking with the mouse
bool RawLook() { return !fcip::Get().mouse_smoothing && fcip::MouseLookActive() && K::KbmActive(); }

void MouseLook(uint32_t cmd) {
    uint64_t now = NowMs();
    uint64_t prev = g_gameplay_ms.exchange(now);
    // drained every frame: motion banked while the look was off is stale and snaps the view when applied late
    double dx = 0;
    double dy = 0;
    K::TakeMouse(dx, dy);
    bool running = prev && now - prev <= kGameplayTimeoutMs;
    if (!cmd || !running || !fcip::MouseLookActive() || (dx == 0 && dy == 0)) return;
    // Steam turns the pad's stick into mouse motion, the pad already looks
    if (K::PadRecent()) return;
    K::NoteKbm();
    const fcip::Settings& s = fcip::Get();
    float k = kDegreesPerPixel * s.sensitivity;
    float yaw = std::clamp(float(dx * k), -kMaxDeltaDegrees, kMaxDeltaDegrees);
    float pitch = std::clamp(float((s.invert_y ? -dy : dy) * k), -kMaxDeltaDegrees, kMaxDeltaDegrees);
    WF(cmd + kCmdYaw, GF(cmd + kCmdYaw) + yaw);
    WF(cmd + kCmdPitch, GF(cmd + kCmdPitch) + pitch);
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
    if (walk) scale *= fcip::Get().walk_speed;
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
void WheelSlot(uint32_t player, uint32_t cmd, uint32_t avail, int steps) {
    if (R32(cmd + kCmdActions) & kSlotBits) return;
    static constexpr uint32_t kOrder[] = {2, 1, 3};  // pistol, rifle, heavy: the order of keys 2, 3 and 4
    int dir = steps > 0 ? -1 : 1;  // wheel down is next
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

// hold to aim sets ZOOM_TOGGLE for one frame only when the key and the zoom disagree; if the game drops the zoom
// itself (reload) the key must be pressed again
void HoldToAim(uint32_t player, uint32_t cmd) {
    static bool was_in = false;
    static bool latched = false;
    static bool exiting = false;
    static uint64_t retry_ms = 0;
    uint32_t zoom = R32(player + kPlayerZoom);
    bool in = GuestOk(zoom) && R8(zoom + kZoomOn);
    bool leaving = in && R8(zoom + kZoomLeaving);
    bool held = Held(K::kAim);
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
    if (MenuOpen() || GetForegroundWindow() != static_cast<HWND>(app::Window())) return false;
    uint64_t stamp = g_gameplay_ms;
    return stamp && NowMs() - stamp <= kGameplayTimeoutMs;
}

void PressStart() {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    g_start_until = until.time_since_epoch().count();
}

void GamePad(int port, input::Pad& pad) {
    if (port != 0) return;
    bool start = std::chrono::steady_clock::now().time_since_epoch().count() < g_start_until;
    K::TrackKeys(pad.connected && !Neutral(pad), MenuOpen());
    g_pad_y = pad.buttons & input::kY;
    if (MenuOpen()) {
        g_reload_only = g_interact_only = false;
        pad = {.connected = true};
        return;
    }
    input::Pad kb = Keyboard(pad);
    // keyboard and mouse ride on port 0, a controller or not
    pad.connected = true;
    pad.buttons |= kb.buttons;
    pad.left_trigger = std::max(pad.left_trigger, kb.left_trigger);
    pad.right_trigger = std::max(pad.right_trigger, kb.right_trigger);
    pad.lx = Larger(pad.lx, kb.lx);
    pad.ly = Larger(pad.ly, kb.ly);
    pad.rx = Larger(pad.rx, kb.rx);
    pad.ry = Larger(pad.ry, kb.ry);
    if (start) pad.buttons |= input::kStart;
}

void GameRumble(int, uint16_t& left, uint16_t& right) {
    if (K::KbmActive()) left = right = 0;
}

bool CursorMessage(void* hwnd, uint32_t msg, intptr_t lp, intptr_t* result) {
    static bool locked = false;
    bool look = MouseLookActive();
    if (look != locked && (msg == WM_INPUT || msg == WM_SETCURSOR || msg == WM_MOUSEMOVE)) {
        locked = look;
        RECT r;
        GetClientRect(static_cast<HWND>(hwnd), &r);
        POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(static_cast<HWND>(hwnd), &centre);
        RECT hold{centre.x, centre.y, centre.x + 1, centre.y + 1};
        ClipCursor(look ? &hold : nullptr);
    }
    if (msg == WM_KILLFOCUS) {
        locked = false;
        ClipCursor(nullptr);
    }
    if (msg != WM_SETCURSOR || !locked || LOWORD(lp) != HTCLIENT) return false;
    SetCursor(nullptr);
    *result = TRUE;
    return true;
}

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ProcessMovements));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_SelectWeaponSlot));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ReloadUse));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Zoom));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Fire));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_Feral));
PPC_EXTERN_FUNC(GUEST_ORIG(CAimHelper_AutoAim));
PPC_EXTERN_FUNC(GUEST_ORIG(CAimHelper_Update));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_UpdateLookBones));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_UpdateView));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_ProcessView));

// (this, cmd): clamps the move vector and applies the walk key
PPC_FUNC(GUEST(CPlayer_ProcessMovements)) {
    if (uint32_t cmd = uint32_t(ctx.r[4])) OnMovement(uint32_t(ctx.r[3]), cmd);
    GUEST_ORIG(CPlayer_ProcessMovements)(ctx, base);
}

// (player, cmd, u8 avail[3]), before ProcessMovements: the wheel presses the next slot the game can switch to
PPC_FUNC(GUEST(CPlayer_SelectWeaponSlot)) {
    uint32_t player = uint32_t(ctx.r[3]);
    uint32_t cmd = uint32_t(ctx.r[4]);
    uint32_t avail = uint32_t(ctx.r[5]);
    if (cmd && cmd == g_local_cmd) {
        // one slot per frame; notches older than a moment (no slot handler in a vehicle) are dropped
        int steps = K::TakeWheel();
        if (steps && K::WheelFresh() && !K::PadRecent() && GuestOk(player) && GuestOk(avail)) {
            K::NoteKbm();
            WheelSlot(player, cmd, avail, steps);
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
    } else if (Local(player, cmd) && fcip::Get().hold_aim && K::KbmActive() && !K::PadRecent()) {
        HoldToAim(player, cmd);
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
    uint32_t player = uint32_t(ctx.r[4]);
    bool off = fcip::Get().auto_helpers && K::KbmActive() && GuestOk(player);
    uint8_t saved = off ? R8(player + kPlayerAutoAim) : 0;
    if (off) W8(player + kPlayerAutoAim, 0);
    GUEST_ORIG(CAimHelper_AutoAim)(ctx, base);
    if (off) W8(player + kPlayerAutoAim, saved);
}

// (helper, cmd): mouse look adds this frame's delta to the look the pad produced; the aiming helper is off for the
// call while the keyboard or mouse is in use
PPC_FUNC(GUEST(CAimHelper_Update)) {
    uint32_t cmd = uint32_t(ctx.r[4]);
    g_local_cmd = cmd;
    HelperOverride helper = SuppressAimingHelper();
    GUEST_ORIG(CAimHelper_Update)(ctx, base);
    if (helper.slot) W32(helper.slot, helper.saved);
    MouseLook(cmd);
}

// (this), local player only: with smoothing off on the mouse, a second pass sees no pitch change, so the lag stays
// zero and the lean steps once
PPC_FUNC(GUEST(CPlayer_UpdateLookBones)) {
    uint32_t player = uint32_t(ctx.r[3]);
    bool raw = player && RawLook();
    float lean = raw ? GF(player + kPlayerLean) : 0.0f;
    GUEST_ORIG(CPlayer_UpdateLookBones)(ctx, base);
    if (!raw) return;
    WF(player + kPlayerLean, lean);
    WF(player + kPlayerPitchLag, 0.0f);
    ctx.r[3] = player;
    GUEST_ORIG(CPlayer_UpdateLookBones)(ctx, base);
}

// (this): its weapon yaw lag, zeroed like the pitch one
PPC_FUNC(GUEST(CPlayer_UpdateView)) {
    uint32_t player = uint32_t(ctx.r[3]);
    GUEST_ORIG(CPlayer_UpdateView)(ctx, base);
    if (!player || player != g_local_player || !RawLook()) return;
    WF(player + kPlayerYawDelta, 0.0f);
    WF(player + kPlayerYawLag, 0.0f);
}

// (this, cmd) applies this frame's look to the entity after the camera was built from last frame's; the same turn
// goes onto the camera before it is copied for rendering, so the mouse shows up the frame it moved
PPC_FUNC(GUEST(CPlayer_ProcessView)) {
    uint32_t player = uint32_t(ctx.r[3]);
    g_local_player = player;
    uint32_t ent = player && RawLook() ? R32(player + kPlayerEntity) : 0;
    bool raw = GuestOk(ent);
    float pitch = raw ? GF(ent + kEntityAngles) : 0.0f;
    float yaw = raw ? GF(ent + kEntityAngles + 8) : 0.0f;
    GUEST_ORIG(CPlayer_ProcessView)(ctx, base);
    if (!raw) return;
    uint32_t cam = R32(ent + kEntityCamera);
    if (!GuestOk(cam)) return;
    float turn = GF(ent + kEntityAngles + 8) - yaw;
    turn -= 360.0f * std::round(turn / 360.0f);
    uint32_t angles = cam + kCameraAngles;
    WF(angles, GF(angles) + GF(ent + kEntityAngles) - pitch);
    WF(angles + 8, GF(angles + 8) + turn);
}
