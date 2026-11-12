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
//
// Past 1x shadow distance sharper maps are drawn first, at 1x, 2x, 4x and on of the stock radius while they divide the
// distance, each a whole run of the update into another pool surface (the pool frees its surfaces after the camera
// pass), so detail halves from one map to the next. Every sun camera sits where the stock radius puts it, on the far
// map's texel grid, with the far map's depth range, and is as wide as its radius. The far map draws last, so the
// receivers keep its texture and matrix. The renderer finds the maps by their resolve addresses and reads the sharpest
// one that covers the point.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "common/draw_distance.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "kernel/memory.h"
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
constexpr uint32_t kFetchAddress = 4;  // fetch constant dword 1: the base from bit 12
constexpr uint32_t kFetchSize = 8;     // fetch constant dword 2: width - 1 and height - 1, 13 bits each
constexpr uint32_t kTitleHeight = 720;  // the guest frame the render scale is taken from
// CCamera: position, view axes, far plane and its copy, the orthographic projection's size
constexpr uint32_t kCameraPosition = 0x4;
constexpr uint32_t kCameraForward = 0x1C;
constexpr uint32_t kCameraUp = 0x28;
constexpr uint32_t kCameraFar = 0x38;
constexpr uint32_t kCameraFarCopy = 0x360;
constexpr uint32_t kCameraOrthoWidth = 0x33C;
constexpr uint32_t kCameraOrthoHeight = 0x340;
// the view the sun camera follows: the shadow radius
constexpr uint32_t kViewShadowRadius = 0x34;

// while the surface call binds a rescaled map: the chosen side, the side the texture is backed at, and the slot the
// pool writes the handle to
thread_local uint32_t t_side = 0;
thread_local uint32_t t_backed = 0;
thread_local uint32_t t_handle = 0;
thread_local uint32_t t_pass_type = 0;  // the pass being submitted

// which run of the shadow map update is drawing, and whether it drew the sun's camera
// sharper maps by index, map i at 2^i times the stock radius, then the far one
constexpr int kNoMap = -1;
constexpr int kFarMap = int(fcip::kSunMaps);
thread_local int t_map = kNoMap;
thread_local bool t_sun = false;

std::atomic<uint32_t> g_bases[fcip::kSunMaps];
std::atomic<uint32_t> g_far_base{0};
std::atomic<uint32_t> g_distance{0};  // the far map's radius over the stock one, 0 without sharper maps
std::atomic<uint32_t> g_maps{0};      // sharper maps drawn
std::atomic<uint32_t> g_side{0};      // the sun map's guest side, as ShadowMatrix takes it

// a map's radius over the stock one
uint32_t Reach(int map) { return map == kFarMap ? g_distance.load() : 1u << map; }

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

void WF(uint32_t va, float f) { W32(va, std::bit_cast<uint32_t>(f)); }

struct Vec {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec GV(uint32_t va) { return {GF(va), GF(va + 4), GF(va + 8)}; }

Vec Unit(Vec v) {
    double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / n, v.y / n, v.z / n};
}

double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// as the game's snap: the position moved down to a whole number of texels along the axis
void SnapAlong(Vec& p, Vec axis, double texel) {
    double d = Dot(p, axis);
    double off = d - std::floor(d / texel) * texel;
    p = {p.x - axis.x * off, p.y - axis.y * off, p.z - axis.z * off};
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
PPC_EXTERN_FUNC(GUEST_ORIG(ShadowMaps_Update));

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

// sun shadow camera, texel snapping, r4 the view, r5 r6 the size, r7 the camera
PPC_FUNC(GUEST(Shadow_Snap)) {
    uint32_t view = uint32_t(ctx.r[4]);
    uint32_t cam = uint32_t(ctx.r[7]);
    Pair(ctx, 5, 6);
    t_sun = true;
    g_side = uint32_t(ctx.r[5]);
    uint32_t distance = g_distance.load();
    if (t_map == kNoMap || !distance) {
        GUEST_ORIG(Shadow_Snap)(ctx, base);
        return;
    }
    uint64_t args[6];
    std::memcpy(args, &ctx.r[3], sizeof(args));
    // the far map's depth range first, then the stock centre on the stock texel grid
    GUEST_ORIG(Shadow_Snap)(ctx, base);
    float far_plane = GF(cam + kCameraFar);
    float radius = GF(view + kViewShadowRadius);
    WF(view + kViewShadowRadius, radius / float(distance));
    std::memcpy(&ctx.r[3], args, sizeof(args));
    GUEST_ORIG(Shadow_Snap)(ctx, base);
    WF(view + kViewShadowRadius, radius);

    double width = GF(cam + kCameraOrthoWidth);
    double far_texel = width * distance / double(uint32_t(args[2]));
    Vec forward = Unit(GV(cam + kCameraForward));
    Vec up = Unit(GV(cam + kCameraUp));
    Vec right = Unit({forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z,
                      forward.x * up.y - forward.y * up.x});
    Vec p = GV(cam + kCameraPosition);
    SnapAlong(p, right, far_texel);
    SnapAlong(p, up, far_texel);
    WF(cam + kCameraPosition, float(p.x));
    WF(cam + kCameraPosition + 4, float(p.y));
    WF(cam + kCameraPosition + 8, float(p.z));
    float side = float(width * Reach(t_map));
    WF(cam + kCameraOrthoWidth, side);
    WF(cam + kCameraOrthoHeight, side);
    WF(cam + kCameraFar, far_plane);
    WF(cam + kCameraFarCopy, far_plane);
    CallGuest(ctx, SYM_CCamera_Update, {cam});
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

// pass end, resolves the bound targets: a depth only pass resolves its depth into the bound depth texture
PPC_FUNC(GUEST(Shadow_Resolve)) {
    uint32_t tex = R32(SYM_RenderTargets_Depth);
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
    if (!ok || w != h || !t_sun || t_map == kNoMap) return;
    uint32_t at = mem::PhysicalFromVirtual(R32(tex + XDK_Texture_FetchConstant + kFetchAddress) & 0xFFFFF000);
    (t_map == kFarMap ? g_far_base : g_bases[t_map]) = at;
}

namespace {

// one run of the update, args kept for the next
void RunShadowMaps(PPCContext& ctx, uint8_t* base, int map) {
    uint64_t args[8];
    std::memcpy(args, &ctx.r[3], sizeof(args));
    double f1 = ctx.f[1];
    double f2 = ctx.f[2];
    t_map = map;
    t_sun = false;
    GUEST_ORIG(ShadowMaps_Update)(ctx, base);
    t_map = kNoMap;
    std::memcpy(&ctx.r[3], args, sizeof(args));
    ctx.f[1] = f1;
    ctx.f[2] = f2;
}

// a sharper map before the far one, with its own slope bias
void RunCascade(PPCContext& ctx, uint8_t* base, int map) {
    fcip::UseNearSunSlope(true, Reach(map));
    RunShadowMaps(ctx, base, map);
    fcip::UseNearSunSlope(false, Reach(map));
}

}  // namespace

uint32_t fcip::SubmittedPass() { return t_pass_type; }

void fcip::DrawShadowMaps(PPCContext& ctx, uint8_t* base) {
    uint32_t distance = Get().shadow_distance;
    bool quad = (ctx.r[6] & 0xFF) != 0;  // four light frustums, no sun map
    // the far map's texel grid must hold whole texels of each sharper map
    uint32_t maps = 0;
    while (!quad && maps < kSunMaps && (1u << maps) < distance && distance % (1u << maps) == 0) maps++;
    g_distance = maps ? distance : 0;
    g_maps = maps;
    for (uint32_t i = 0; i < maps; i++) RunCascade(ctx, base, int(i));
    RunShadowMaps(ctx, base, kFarMap);
}

fcip::SunMaps fcip::SunMapsNow() {
    SunMaps m;
    m.far_base = g_far_base;
    m.count = g_maps;
    m.distance = g_distance;
    m.side = g_side;
    for (uint32_t i = 0; i < m.count; i++) m.bases[i] = g_bases[i];
    return m;
}

uint32_t fcip::SunShadowSide() {
    uint32_t side = Wanted(kStockSide);
    return side ? side : kStockSide;
}
