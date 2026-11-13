// Sun shadow map resolution. The shadow map update picks a fixed pixel budget, 0x1B9000 for one frustum or 0x6E400
// for four, and takes its square root: 1344 or 672 per side. That value reaches five places, each hooked to rescale
// the side where it matches the stock pair: the depth surface (its pool is keyed on width, height and format, so a
// new size allocates), the viewport with 1/w and 1/h, the shadow camera's texel snapping, the draw extent, and the
// pass struct's own copy. The fetch constant packs each dimension into 13 bits, so 8192 is the ceiling.
//
// The setting is the map's guest side; the renderer draws it at the render scale like every target, short of the host's
// 16384 texture edge, so its texels shrink with the output as the screen's do. Automatic is the stock side. The four
// maps drawn inside a vis area take half the side, as the stock 672 against 1344.
//
// Only the host reads a map, so its guest texture is never backed above the stock side: larger ones outgrow guest
// physical memory (144 MB at 6144), and a cascade takes six. Its header carries the chosen size from creation on: the
// receivers take their texel offsets from it and the renderer sizes its copy from it. A new side leaves the old
// surfaces in the pool, which frees unused ones only when the screen size changes, so it is told the screen changed.
//
// Past 1x shadow distance sharper maps are drawn first, at 1x, 2x, 4x and 8x of the stock radius while they divide the
// distance, each a whole run of the update into another pool surface (the pool frees its surfaces after the camera
// pass), so detail halves from one map to the next. Every sun camera sits where the stock radius puts it, on the far
// map's texel grid, with the far map's depth range, and is as wide as its radius. The far map draws last, so the
// receivers keep its texture and matrix. The renderer finds the maps by their resolve addresses and reads the sharpest
// one that covers the point. Inside a vis area the far run draws the game's four light frustums, the sun among them;
// the sharper runs draw one frustum, the top scored light, normally the sun. Only the sun's resolves are recorded.
//
// Each sun camera culls its casters to the part of its map a receiver can read: receivers lie in the view frustum, or
// with water in its reflection, and a caster only shades what it covers along the sun. The camera's side planes are
// pulled in to that span; its matrices keep the whole map, so only the engine's culling sees the narrower box.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/draw_distance.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/motion.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace {

constexpr uint32_t kStockSide = 1344;     // one frustum
constexpr uint32_t kStockSideQuad = 672;  // four frustums
constexpr uint32_t kMaxSide = 8192;
constexpr uint32_t kPassWidth = 0x1C;
constexpr uint32_t kPassHeight = 0x20;
constexpr uint32_t kPassType = 0x10;
constexpr uint32_t kPassReflection = 1;      // types 0 and 1 draw the water's reflections
constexpr uint32_t kPassWaterHeight = 0x30;  // the height a reflection clips at
constexpr uint32_t kFetchAddress = 4;  // fetch constant dword 1: the base from bit 12
constexpr uint32_t kFetchSize = 8;     // fetch constant dword 2: width - 1 and height - 1, 13 bits each
// CCamera: position, view axes, far plane and its copy, the orthographic projection's size
constexpr uint32_t kCameraPosition = 0x4;
constexpr uint32_t kCameraForward = 0x1C;
constexpr uint32_t kCameraUp = 0x28;
constexpr uint32_t kCameraNear = 0x34;
constexpr uint32_t kCameraFar = 0x38;
constexpr uint32_t kCameraFarCopy = 0x360;
constexpr uint32_t kCameraOrthoWidth = 0x33C;
constexpr uint32_t kCameraOrthoHeight = 0x340;
constexpr uint32_t kCameraFov = 0x3C;  // horizontal radians, narrower while zoomed
constexpr float kHipFov = 1.5707964f;  // the stock 90 degrees outside zoom
constexpr uint32_t kCameraProjection = 0x23C;  // row major: x and y scale at 0 and 5, centre offsets at 8 and 9
// six frustum planes {n, d}, a point inside where n.x + d <= 0, then the same six with d negated for the box tests
constexpr uint32_t kCameraPlanes = 0x44;
constexpr uint32_t kCameraPlanesNegated = 0xB4;
constexpr uint32_t kPlaneCount = 6;
constexpr uint32_t kPlaneSize = 0x10;
constexpr uint32_t kPlaneD = 0xC;
constexpr double kSideCos = 0.99;      // a side plane's normal lies along the map's right or up
constexpr double kReceiverTexels = 3;  // the receivers' taps reach past the texel they fall on
// the view the sun camera follows: its camera and the shadow radius
constexpr uint32_t kViewCamera = 0x0;
constexpr uint32_t kViewShadowRadius = 0x34;
// the renderer's surface pool frees its unused surfaces when the screen width it last saw changes
constexpr uint32_t kRendererTargets = 0x4;
constexpr uint32_t kTargetsPool = 0x25E4;
constexpr uint32_t kPoolScreenWidth = 0x28;
constexpr uint32_t kNoScreen = 0xFFFFFFFF;

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

std::atomic<uint32_t> g_distance{0};  // the far map's radius over the stock one, 0 without sharper maps
std::atomic<uint32_t> g_maps{0};      // sharper maps drawn
std::atomic<uint32_t> g_side{0};      // the sun map's guest side, as ShadowMatrix takes it
uint32_t g_pool_side = 0;             // the sun map side the pool's surfaces were made for
std::atomic<float> g_water{std::numeric_limits<float>::quiet_NaN()};  // the last reflection's water height

// sun map resolves in the order the title issues them; the render thread runs up to a frame behind, so it matches each
// of its own resolves to the first unmatched record of that address and reads the maps as they were drawn
struct SunResolve {
    uint32_t address = 0;
    int map = kNoMap;
    uint32_t count = 0;
    uint32_t distance = 0;
    uint32_t side = 0;
};
constexpr uint32_t kResolveRing = 64;  // several frames of sun map resolves
SunResolve g_ring[kResolveRing];
std::atomic<uint32_t> g_issued{0};
uint32_t g_matched = 0;   // render thread
fcip::SunMaps g_drawn;    // render thread

// a map's radius over the stock one
uint32_t Reach(int map) { return map == kFarMap ? g_distance.load() : 1u << map; }

// the replacement side, 0 when this is not the shadow map or the side stays
uint32_t Wanted(uint32_t side) {
    if (side != kStockSide && side != kStockSideQuad) return 0;
    uint32_t edge = fcip::Get().shadow_resolution;
    if (edge == 0) return 0;
    uint64_t want = side == kStockSideQuad ? edge / 2 : edge;
    uint64_t scaled = std::min<uint64_t>((want + 7) & ~uint64_t(7), kMaxSide);
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

Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// a + b * s
Vec Madd(Vec a, Vec b, double s) { return {a.x + b.x * s, a.y + b.y * s, a.z + b.z * s}; }

// an interval along one of the sun map's axes
struct Span {
    double lo = std::numeric_limits<double>::max();
    double hi = -std::numeric_limits<double>::max();

    void Add(double t) {
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
};

// as the game's snap: the position moved down to a whole number of texels along the axis
void SnapAlong(Vec& p, Vec axis, double texel) {
    double d = Dot(p, axis);
    double off = d - std::floor(d / texel) * texel;
    p = {p.x - axis.x * off, p.y - axis.y * off, p.z - axis.z * off};
}

// a square side in registers a and b
void Pair(PPCContext& ctx, int a, int b) {
    uint32_t w = uint32_t(ctx.r[a]);
    if (w != uint32_t(ctx.r[b])) return;
    if (uint32_t side = Wanted(w)) ctx.r[a] = ctx.r[b] = side;
}

// the view frustum's eight corners
bool ViewCorners(uint32_t eye, Vec (&at)[8]) {
    uint32_t proj = eye + kCameraProjection;
    double scale_x = std::abs(GF(proj));
    double scale_y = std::abs(GF(proj + 5 * 4));
    double near_plane = GF(eye + kCameraNear);
    double far_plane = GF(eye + kCameraFar);
    if (!(scale_x > 0 && scale_y > 0 && near_plane >= 0 && far_plane > 0)) return false;
    // an off centre projection widens either side
    double tan_x = (1 + std::abs(GF(proj + 8 * 4))) / scale_x;
    double tan_y = (1 + std::abs(GF(proj + 9 * 4))) / scale_y;
    Vec forward = Unit(GV(eye + kCameraForward));
    Vec right = Unit(Cross(forward, GV(eye + kCameraUp)));
    Vec up = Cross(right, forward);
    Vec p = GV(eye + kCameraPosition);
    for (int i = 0; i < 4; i++) {
        Vec dir = Madd(Madd(forward, right, i & 1 ? tan_x : -tan_x), up, i & 2 ? tan_y : -tan_y);
        at[i] = Madd(p, dir, near_plane);
        at[i + 4] = Madd(p, dir, far_plane);
    }
    return true;
}

// the sun camera's side planes pulled in to the view's span, see the top of the file
void CullToView(uint32_t view, uint32_t cam, uint32_t map_side) {
    Vec corners[8];
    if (!map_side || !ViewCorners(R32(view + kViewCamera), corners)) return;
    Vec forward = Unit(GV(cam + kCameraForward));
    Vec right = Unit(Cross(forward, GV(cam + kCameraUp)));
    Vec up = Cross(right, forward);
    Vec centre = GV(cam + kCameraPosition);
    Span across;
    Span down;
    auto add = [&](Vec point) {
        Vec offset = Madd(point, centre, -1);
        across.Add(Dot(right, offset));
        down.Add(Dot(up, offset));
    };
    float water = g_water;
    for (Vec c : corners) {
        add(c);
        if (!std::isnan(water)) add({c.x, c.y, 2.0 * water - c.z});
    }
    double margin = kReceiverTexels * GF(cam + kCameraOrthoWidth) / map_side;
    for (Span* s : {&across, &down}) {
        s->lo -= margin;
        s->hi += margin;
    }
    for (uint32_t i = 0; i < kPlaneCount; i++) {
        uint32_t at = cam + kCameraPlanes + i * kPlaneSize;
        Vec n = GV(at);
        double k_right = Dot(n, right);
        double k_up = Dot(n, up);
        bool on_right = std::abs(k_right) > std::abs(k_up);
        double k = on_right ? k_right : k_up;
        // the near and far planes face the sun
        if (std::abs(k) < kSideCos * std::sqrt(Dot(n, n))) continue;
        const Span& s = on_right ? across : down;
        double edge = -(Dot(n, centre) + GF(at + kPlaneD)) / k;
        double want = k > 0 ? std::min(edge, s.hi) : std::max(edge, s.lo);
        double d = -k * want - Dot(n, centre);
        WF(at + kPlaneD, float(d));
        WF(cam + kCameraPlanesNegated + i * kPlaneSize + kPlaneD, float(-d));
    }
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

// depth surface create or fetch, vtable +0x1E0, r4 the renderer, r7 r8 the size
PPC_FUNC(GUEST(Shadow_Surface)) {
    uint32_t side = fcip::SunShadowSide();
    if (side != g_pool_side) {
        uint32_t pool = R32(R32(uint32_t(ctx.r[4]) + kRendererTargets) + kTargetsPool);
        if (g_pool_side) W32(pool + kPoolScreenWidth, kNoScreen);
        g_pool_side = side;
    }
    uint32_t stock = uint32_t(ctx.r[7]);
    Pair(ctx, 7, 8);
    uint32_t w = uint32_t(ctx.r[7]);
    if (w != stock) {
        t_side = w;
        t_backed = std::min(w, stock);
        t_handle = uint32_t(ctx.r[3]);
    }
    GUEST_ORIG(Shadow_Surface)(ctx, base);
    t_side = 0;
}

// texture create under the pool miss, r3 r4 width and height
PPC_FUNC(GUEST(Shadow_Texture)) {
    bool backed = t_side && t_backed != t_side && uint32_t(ctx.r[3]) == t_side && uint32_t(ctx.r[4]) == t_side;
    if (backed) ctx.r[3] = ctx.r[4] = t_backed;
    GUEST_ORIG(Shadow_Texture)(ctx, base);
    uint32_t tex = uint32_t(ctx.r[3]);
    if (!backed || tex < 0x10000u || tex >= 0xC0000000u) return;
    uint32_t size = tex + XDK_Texture_FetchConstant + kFetchSize;
    W32(size, (R32(size) & ~0x3FFFFFFu) | (t_side - 1) | (t_side - 1) << 13);
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
        uint32_t map_side = uint32_t(ctx.r[5]);
        GUEST_ORIG(Shadow_Snap)(ctx, base);
        CullToView(view, cam, map_side);
        return;
    }
    uint64_t args[6];
    std::memcpy(args, &ctx.r[3], sizeof(args));
    // the snap widens the map by the view's angle, so zoom would pull the far map in
    uint32_t view_cam = R32(view + kViewCamera);
    float fov = GF(view_cam + kCameraFov);
    WF(view_cam + kCameraFov, std::max(fov, kHipFov));
    // the far map's depth range first, then the stock centre on the stock texel grid
    GUEST_ORIG(Shadow_Snap)(ctx, base);
    float far_plane = GF(cam + kCameraFar);
    float radius = GF(view + kViewShadowRadius);
    WF(view + kViewShadowRadius, radius / float(distance));
    std::memcpy(&ctx.r[3], args, sizeof(args));
    GUEST_ORIG(Shadow_Snap)(ctx, base);
    WF(view + kViewShadowRadius, radius);
    WF(view_cam + kCameraFov, fov);

    double width = GF(cam + kCameraOrthoWidth);
    double far_texel = width * distance / double(uint32_t(args[2]));
    Vec forward = Unit(GV(cam + kCameraForward));
    Vec up = Unit(GV(cam + kCameraUp));
    Vec right = Unit(Cross(forward, up));
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
    CullToView(view, cam, uint32_t(args[2]));
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
    if (ok && t_pass_type <= kPassReflection) g_water = GF(pass + kPassWaterHeight);
    uint32_t w = ok ? R32(pass + kPassWidth) : 0;
    uint32_t h = ok ? R32(pass + kPassHeight) : 0;
    uint32_t side = ok && w == h ? Wanted(w) : 0;
    if (side) {
        W32(pass + kPassWidth, side);
        W32(pass + kPassHeight, side);
    }
    // DLSS draws the camera's pass at its own scale and upscales it as the pass ends; the editor names no device
#if HAS_Device_pointer
    bool scene = t_pass_type == fcip::kPassCamera && fcip::Get().dlaa;
    if (scene) d3d::SceneBegin(R32(SYM_Device_pointer));
    GUEST_ORIG(Shadow_Pass)(ctx, base);
    if (scene) d3d::SceneEnd(R32(SYM_Device_pointer));
#else
    GUEST_ORIG(Shadow_Pass)(ctx, base);
#endif
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
    // recorded before the resolve is queued, so the render thread finds it
    if (ok && w == h && t_sun && t_map != kNoMap) {
        uint32_t at = mem::PhysicalFromVirtual(R32(tex + XDK_Texture_FetchConstant + kFetchAddress) & 0xFFFFF000);
        uint32_t n = g_issued.load(std::memory_order_relaxed);
        g_ring[n % kResolveRing] = {at, t_map, g_maps, g_distance, g_side};
        g_issued.store(n + 1, std::memory_order_release);
        t_sun = false;  // the lights after the sun in a quad run
    }
    GUEST_ORIG(Shadow_Resolve)(ctx, base);
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
    uint32_t distance = Get().draw_distance;
    // the far map's texel grid must hold whole texels of each sharper map
    uint32_t maps = 0;
    while (maps < kSunMaps && (1u << maps) < distance && distance % (1u << maps) == 0) maps++;
    g_distance = maps ? distance : 0;
    g_maps = maps;
    uint64_t quad = ctx.r[6];
    ctx.r[6] = 0;  // one frustum: the sharper maps hold only the sun
    for (uint32_t i = 0; i < maps; i++) RunCascade(ctx, base, int(i));
    ctx.r[6] = quad;
    RunShadowMaps(ctx, base, kFarMap);
}

int fcip::SunResolved(uint32_t address) {
    uint32_t issued = g_issued.load(std::memory_order_acquire);
    if (issued - g_matched > kResolveRing) g_matched = issued - kResolveRing;
    for (uint32_t i = g_matched; i != issued; i++) {
        const SunResolve& r = g_ring[i % kResolveRing];
        if (r.address != address) continue;
        g_matched = i + 1;
        if (r.map != kFarMap) {
            g_drawn.bases[r.map] = address;
            g_drawn.near_side = r.side;
            return r.map;
        }
        g_drawn.far_base = address;
        g_drawn.count = r.count;
        g_drawn.distance = r.distance;
        g_drawn.side = r.side;
        return r.map;
    }
    return kNoMap;
}

fcip::SunMaps fcip::SunMapsDrawn() { return g_drawn; }

uint32_t fcip::SunShadowSide() {
    uint32_t side = Wanted(kStockSide);
    return side ? side : kStockSide;
}
