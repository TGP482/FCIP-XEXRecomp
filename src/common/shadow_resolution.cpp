// Sun shadow map resolution. The shadow map update picks a fixed pixel budget, 0x1B9000 for one frustum or 0x6E400
// for four, and takes its square root: 1344 or 672 per side. That value reaches five places, each hooked to rescale
// the side where it matches the stock pair: the depth surface (its pool is keyed on width, height and format, so a
// new size allocates), the viewport with 1/w and 1/h, the shadow camera's texel snapping, the draw extent, and the
// pass struct's own copy. The fetch constant packs each dimension into 13 bits, so 8192 is the ceiling.
//
// Above 4096 the map outgrows guest physical memory (144 MB at 6144, 256 MB at 8192): the texture create fails, the
// sun pass is skipped and receivers keep the last map under a new matrix. There the texture is backed at the stock
// side and reports the chosen one; the pass rect still sizes the surface and resolve, and the renderer's host texture
// follows the resolve rect.
#include <algorithm>

#include "common/settings.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kStockSide = 1344;     // one frustum
constexpr uint32_t kStockSideQuad = 672;  // four frustums
constexpr uint32_t kMaxSide = 8192;
constexpr uint32_t kMaxBacked = 4096;  // the largest setting whose maps fit in guest memory
constexpr uint32_t kPassWidth = 0x1C;
constexpr uint32_t kPassHeight = 0x20;

// while the surface call binds a rescaled map: the chosen side, the side the texture is backed at, and the slot the
// pool writes the handle to
thread_local uint32_t t_side = 0;
thread_local uint32_t t_backed = 0;
thread_local uint32_t t_handle = 0;

// the replacement side, 0 when this is not the shadow map
uint32_t Wanted(uint32_t side) {
    uint32_t want = fcip::Get().shadow_resolution;
    if (want == 0 || want == kStockSide) return 0;
    if (side != kStockSide && side != kStockSideQuad) return 0;
    uint64_t scaled = (uint64_t(side) * want / kStockSide + 7) & ~uint64_t(7);
    scaled = std::min<uint64_t>(scaled, kMaxSide);
    return scaled == side ? 0 : uint32_t(scaled);
}

// a square side in registers a and b
void Pair(PPCContext& ctx, int a, int b) {
    uint32_t w = uint32_t(ctx.r[a]);
    if (w != uint32_t(ctx.r[b])) return;
    if (uint32_t side = Wanted(w)) ctx.r[a] = ctx.r[b] = side;
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Surface));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Texture));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Size));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Viewport));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Snap));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Extent));
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Pass));

// depth surface create or fetch, vtable +0x1E0, r7 r8 the size
PPC_FUNC(GUEST(Shadow_Surface)) {
    uint32_t stock = uint32_t(ctx.r[7]);
    Pair(ctx, 7, 8);
    uint32_t w = uint32_t(ctx.r[7]);
    if (w != stock) {
        t_side = w;
        t_backed = uint64_t(w) * kStockSide > uint64_t(kMaxBacked) * stock ? stock : w;
        t_handle = uint32_t(ctx.r[3]);
    }
    GUEST_ORIG(Shadow_Surface)(ctx, base);
    t_side = 0;
}

// texture create under the pool miss, r3 r4 width and height
PPC_FUNC(GUEST(Shadow_Texture)) {
    if (t_side && t_backed != t_side && uint32_t(ctx.r[3]) == t_side && uint32_t(ctx.r[4]) == t_side)
        ctx.r[3] = ctx.r[4] = t_backed;
    GUEST_ORIG(Shadow_Texture)(ctx, base);
}

// size by handle, r5 r6 point at width and height; the bind (vtable +0x1EC) sets the viewport and rect from it
PPC_FUNC(GUEST(Shadow_Size)) {
    uint32_t handle = uint32_t(ctx.r[4]);
    uint32_t pw = uint32_t(ctx.r[5]);
    uint32_t ph = uint32_t(ctx.r[6]);
    GUEST_ORIG(Shadow_Size)(ctx, base);
    if (!t_side || handle != R32(t_handle)) return;
    W32(pw, t_side);
    W32(ph, t_side);
}

// viewport and inverse size, vtable +0x100
PPC_FUNC(GUEST(Shadow_Viewport)) {
    Pair(ctx, 7, 8);
    GUEST_ORIG(Shadow_Viewport)(ctx, base);
}

// sun shadow camera, texel snapping
PPC_FUNC(GUEST(Shadow_Snap)) {
    Pair(ctx, 5, 6);
    GUEST_ORIG(Shadow_Snap)(ctx, base);
}

// draw extent, vtable +0x1E8
PPC_FUNC(GUEST(Shadow_Extent)) {
    Pair(ctx, 4, 5);
    GUEST_ORIG(Shadow_Extent)(ctx, base);
}

// pass submit, vtable +0x2C: the pass carries its own copy of the size
PPC_FUNC(GUEST(Shadow_Pass)) {
    uint32_t pass = uint32_t(ctx.r[4]);
    bool ok = pass >= 0x10000u && pass < 0xC0000000u;
    uint32_t w = ok ? R32(pass + kPassWidth) : 0;
    uint32_t h = ok ? R32(pass + kPassHeight) : 0;
    uint32_t side = ok && w == h ? Wanted(w) : 0;
    if (side) {
        W32(pass + kPassWidth, side);
        W32(pass + kPassHeight, side);
    }
    GUEST_ORIG(Shadow_Pass)(ctx, base);
    if (side) {
        W32(pass + kPassWidth, w);
        W32(pass + kPassHeight, h);
    }
}
