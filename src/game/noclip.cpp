// default.xex's debug page noclip: the noclip key flies the local player through walls with its physics suspended.
// Move and look as usual, jump rises, crouch sinks, feral run and walk or melee step the speed up and down, each
// wrapping so one key alone comes back round to the base speed.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "common/input.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/settings.h"
#include "core/input.h"
#include "game/fpsfixes.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
namespace K = fcip::keys;

namespace {

constexpr uint32_t kPlayerEntity = 0x04;  // CPlayer
// CEntity
constexpr uint32_t kEntityGetPos = 0x10;     // vtable: (this, bool) returns its position
constexpr uint32_t kEntitySetPos = 0x14;     // vtable: (this, const Vec3*, bool)
constexpr uint32_t kEntityCollider = 0x23C;  // EnablePhysics(false) destroys this one for good
// CXEntityProcessingCmd angles in degrees, positive pitch looks down
constexpr uint32_t kCmdPitch = 0x18;
constexpr uint32_t kCmdYaw = 0x20;
constexpr float kYawOffset = 180.0f;  // ProcessMovements turns the cmd's yaw by this
constexpr float kDegrees = 3.14159265f / 180.0f;

constexpr float kBaseSpeed = 12.0f;  // metres a second at the base step
constexpr float kSpeedSteps[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
constexpr int kBaseStep = 3;
constexpr int kStickDead = 7849;

// the pad buttons noclip takes, keys included as they ride on the pad
constexpr uint16_t kRise = input::kA;          // jump
constexpr uint16_t kSink = input::kLeftThumb;  // crouch
constexpr uint16_t kFaster = input::kY;        // feral run
constexpr uint16_t kSlower = input::kB;        // melee

// pad poll writes, the game thread reads
std::atomic<bool> g_on{false};
std::atomic<float> g_strafe{0.0f};
std::atomic<float> g_forward{0.0f};
std::atomic<float> g_rise{0.0f};
std::atomic<int> g_step{kBaseStep};

uint32_t g_entity = 0;  // the entity whose physics noclip suspended

float Stick(int16_t value) { return std::abs(value) < kStickDead ? 0.0f : std::max(value / 32767.0f, -1.0f); }

void StepSpeed(bool faster, bool slower) {
    static bool was_faster = false;
    static bool was_slower = false;
    int count = int(std::size(kSpeedSteps));
    int step = g_step;
    if (faster && !was_faster) step = (step + 1) % count;
    if (slower && !was_slower) step = (step + count - 1) % count;
    g_step = step;
    was_faster = faster;
    was_slower = slower;
}

// port 0 after the keys merged in: the toggle, and while flying the move and the buttons noclip takes
void NoclipPad(input::Pad& pad) {
    static bool was_key = false;
    bool key = K::Held(K::kNoclip);
    if (!fcip::Get().noclip) g_on = false;
    else if (key && !was_key) g_on = !g_on;
    was_key = key;
    if (!g_on || fcip::InMenu()) return;
    uint16_t buttons = pad.buttons;
    StepSpeed(buttons & kFaster, (buttons & kSlower) || K::Held(K::kWalk));
    g_strafe = Stick(pad.lx);
    g_forward = Stick(pad.ly);
    g_rise = float(bool(buttons & kRise)) - float(bool(buttons & kSink));
    pad.buttons = buttons & ~(kRise | kSink | kFaster | kSlower);
    pad.lx = 0;
    pad.ly = 0;
}

// the game's own EnablePhysics: off suspends the player's physical entity, on restores it
bool SetPhysics(PPCContext& ctx, uint32_t entity, bool enable) {
    if (!enable && R32(entity + kEntityCollider)) return false;
    CallGuest(ctx, SYM_Entity_EnablePhysics, {entity, enable ? 1u : 0u});
    return true;
}

void WF(uint32_t addr, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(addr, raw);
}

// moves the entity along the look, as ProcessMovements turns its move: strafe x, forward y, up z about the yaw
void Fly(PPCContext& ctx, uint32_t entity, uint32_t cmd) {
    float distance = kBaseSpeed * kSpeedSteps[g_step] * float(fcip::FrameTime());
    float strafe = g_strafe * distance;
    float forward = g_forward * distance;
    float rise = g_rise * distance;
    if (strafe == 0.0f && forward == 0.0f && rise == 0.0f) return;
    float pitch = GF(cmd + kCmdPitch) * kDegrees;
    float yaw = (GF(cmd + kCmdYaw) + kYawOffset) * kDegrees;
    float ahead = forward * std::cos(pitch);
    float up = rise - forward * std::sin(pitch);
    float c = std::cos(yaw);
    float s = std::sin(yaw);
    uint32_t vtable = R32(entity);
    uint32_t from = uint32_t(CallGuest(ctx, R32(vtable + kEntityGetPos), {entity, 0}));
    static const uint32_t to = mem::AllocSystem(12);
    WF(to, GF(from) + strafe * c - ahead * s);
    WF(to + 4, GF(from + 4) + strafe * s + ahead * c);
    WF(to + 8, GF(from + 8) + up);
    CallGuest(ctx, R32(vtable + kEntitySetPos), {entity, to, 0});
}

// physics comes back where noclip started, as SetPos only moves the physical entity while physics is on; one more
// SetPos at the entity's own position brings it along
void Land(PPCContext& ctx, uint32_t entity) {
    SetPhysics(ctx, entity, true);
    uint32_t vtable = R32(entity);
    uint32_t at = uint32_t(CallGuest(ctx, R32(vtable + kEntityGetPos), {entity, 0}));
    CallGuest(ctx, R32(vtable + kEntitySetPos), {entity, at, 0});
}

// the local player before the camera and the weapon are built from its position, with the cmd of its last movement
void NoclipMove(PPCContext& ctx, uint32_t player, uint32_t cmd) {
    uint32_t entity = R32(player + kPlayerEntity);
    // a new entity left the old one behind, its physics went with it
    if (g_entity != entity) g_entity = 0;
    if (g_entity && !g_on) {
        Land(ctx, g_entity);
        g_entity = 0;
    }
    if (!g_on || !entity) return;
    if (!g_entity && !SetPhysics(ctx, entity, false)) {
        g_on = false;
        return;
    }
    g_entity = entity;
    if (!fcip::MenuOpen() && !fcip::InMenu()) Fly(ctx, entity, cmd);
}

const bool g_registered = (fcip::g_noclip_pad = NoclipPad, fcip::g_noclip_move = NoclipMove, true);

}  // namespace
