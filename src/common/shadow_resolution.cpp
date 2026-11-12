// Sun shadow map resolution. The shadow map update picks a fixed pixel budget, 0x1B9000 for one frustum or 0x6E400
// for four, and takes its square root: 1344 or 672 per side. That value reaches five places, each hooked to rescale
// the side where it matches the stock pair: the depth surface (its pool is keyed on width, height and format, so a
// new size allocates), the viewport with 1/w and 1/h, the shadow camera's texel snapping, the draw extent, and the
// pass struct's own copy. The fetch constant packs each dimension into 13 bits, so 8192 is the ceiling.
//
// The setting is the map's edge on the host, which draws every guest pixel at the render scale, so the guest side is
// the setting over the render scale; 0 keeps the stock side, which then follows the render scale.
//
// Above 4096 the map outgrows guest physical memory (144 MB at 6144, 256 MB at 8192): the texture create fails, the
// sun pass is skipped and receivers keep the last map under a new matrix. There the texture is backed at the stock
// side and reports the chosen one; while the depth is resolved its header carries the chosen size too, as the
// renderer sizes its copy from it.
#include <algorithm>

#include "app/app.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace {

constexpr uint32_t kStockSide = 1344;     // one frustum
constexpr uint32_t kStockSideQuad = 672;  // four frustums
constexpr uint32_t kMaxSide = 8192;
constexpr uint32_t kMaxBacked = 4096;  // the largest guest side whose maps fit in guest memory
constexpr uint32_t kPassWidth = 0x1C;
constexpr uint32_t kPassHeight = 0x20;
constexpr uint32_t kPassType = 0x10;
constexpr uint32_t kFetchSize = 8;  // fetch constant dword 2: width - 1 and height - 1, 13 bits each
constexpr uint32_t kTitleHeight = 720;  // the guest frame the render scale is taken from

// while the surface call binds a rescaled map: the chosen side, the side the texture is backed at, and the slot the
// pool writes the handle to
thread_local uint32_t t_side = 0;
thread_local uint32_t t_backed = 0;
thread_local uint32_t t_handle = 0;
thread_local uint32_t t_pass_type = 0;  // the pass being submitted

// the replacement side, 0 when this is not the shadow map or the setting keeps it
uint32_t Wanted(uint32_t side) {
    uint32_t edge = fcip::Get().shadow_resolution;
    if (edge == 0) return 0;
    if (side != kStockSide && side != kStockSideQuad) return 0;
    double guest = double(side) * edge / (kStockSide * app::RenderScale(kTitleHeight));
    uint64_t scaled = (uint64_t(guest) + 7) & ~uint64_t(7);
    scaled = std::min<uint64_t>(scaled, kMaxSide);
    return scaled == side ? 0 : uint32_t(scaled);
}

// four quad maps take the memory of one full map
bool Fits(uint32_t side, uint32_t stock) { return uint64_t(side) * kStockSide <= uint64_t(kMaxBacked) * stock; }

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
PPC_EXTERN_FUNC(GUEST_ORIG(Shadow_Resolve));

// depth surface create or fetch, vtable +0x1E0, r7 r8 the size
PPC_FUNC(GUEST(Shadow_Surface)) {
    uint32_t stock = uint32_t(ctx.r[7]);
    Pair(ctx, 7, 8);
    uint32_t w = uint32_t(ctx.r[7]);
    if (w != stock) {
        t_side = w;
        t_backed = Fits(w, stock) ? w : stock;
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
    uint32_t outer = t_pass_type;
    t_pass_type = ok ? R32(pass + kPassType) : 0;
    uint32_t w = ok ? R32(pass + kPassWidth) : 0;
    uint32_t h = ok ? R32(pass + kPassHeight) : 0;
    uint32_t side = ok && w == h ? Wanted(w) : 0;
    if (side) {
        W32(pass + kPassWidth, side);
        W32(pass + kPassHeight, side);
    }
    GUEST_ORIG(Shadow_Pass)(ctx, base);
    t_pass_type = outer;
    if (side) {
        W32(pass + kPassWidth, w);
        W32(pass + kPassHeight, h);
    }
}

// colour and depth resolve of the bound targets, r4 the depth texture
PPC_FUNC(GUEST(Shadow_Resolve)) {
    uint32_t tex = uint32_t(ctx.r[4]);
    bool ok = tex >= 0x10000u && tex < 0xC0000000u;
    uint32_t size = tex + XDK_Texture_FetchConstant + kFetchSize;
    uint32_t bits = ok ? R32(size) : 0;
    uint32_t w = (bits & 0x1FFF) + 1;
    uint32_t h = (bits >> 13 & 0x1FFF) + 1;
    uint32_t side = ok && w == h ? Wanted(w) : 0;
    if (side && Fits(side, w)) side = 0;
    if (side) W32(size, (bits & ~0x3FFFFFFu) | (side - 1) | (side - 1) << 13);
    GUEST_ORIG(Shadow_Resolve)(ctx, base);
    if (side) W32(size, bits);
}

uint32_t fcip::SubmittedPass() { return t_pass_type; }

uint32_t fcip::SunShadowSide() {
    uint32_t side = Wanted(kStockSide);
    return side ? side : kStockSide;
}
