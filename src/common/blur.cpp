// The Aim and Sprint Blur option. The player's blur block (+0x994 lock, +0x998 amount, +0x9B8 type) feeds the post
// blur each frame. Zoom in and the zoom steps write an amount of 0.3 or more while scoped, gated on the weapon's
// ZoomDef bBlur and the lock; feral run writes type 1, scaled by player+0x5D8, the only writer of type 1, and fades
// any type 1 blur once the run ends.
#include "common/settings.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kZoomPlayer = 0x94;
constexpr uint32_t kBlurLock = 0x994;
constexpr uint32_t kBlurAmount = 0x998;
constexpr uint32_t kBlurType = 0x9B8;
constexpr uint32_t kRadialType = 1;

bool GuestOk(uint32_t addr) { return addr >= 0x10000u && addr < 0xC0000000u; }

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(CZoom_ZoomIn));
PPC_EXTERN_FUNC(GUEST_ORIG(CPlayer_FeralRunBlur));

// (zoom): the lock keeps the scope blur off
PPC_FUNC(GUEST(CZoom_ZoomIn)) {
    uint32_t zoom = uint32_t(ctx.r[3]);
    uint32_t player = GuestOk(zoom) ? R32(zoom + kZoomPlayer) : 0;
    bool lock = !fcip::Get().blur && GuestOk(player);
    uint8_t saved = lock ? R8(player + kBlurLock) : 0;
    if (lock) W8(player + kBlurLock, 1);
    GUEST_ORIG(CZoom_ZoomIn)(ctx, base);
    if (lock) W8(player + kBlurLock, saved);
}

// (player): feral run blur and FOV; zeroing the amount afterwards also clears a blur left over from turning the
// option off mid run
PPC_FUNC(GUEST(CPlayer_FeralRunBlur)) {
    uint32_t player = uint32_t(ctx.r[3]);
    GUEST_ORIG(CPlayer_FeralRunBlur)(ctx, base);
    if (fcip::Get().blur || !GuestOk(player)) return;
    if (!R8(player + kBlurLock) && R32(player + kBlurType) == kRadialType) W32(player + kBlurAmount, 0);
}
