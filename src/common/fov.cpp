// The field of view: the camera takes the user's true horizontal angle, zoom kept in tan space against the stock 90
// (above 90 the zoom levels keep their stock angles), and above 16:9 the view keeps its 16:9 height and widens. The
// weapon and arms get their own projection from the camera's second angle, scaled the same way by the viewmodel
// setting. Cutscenes keep their own angles.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/aspect.h"
#include "common/input.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// CCamera: m_fov horizontal radians at +0x3C, m_aspect at +0x1B4; Update builds the projection with fovY = m_fov /
// m_aspect, so stock 90 shows about 80 horizontal at 16:9
constexpr uint32_t kCameraFov = 0x3C;
constexpr uint32_t kCameraViewmodelFov = 0x40;  // vertical radians, the near projection at +0x2BC
constexpr uint32_t kCameraAspect = 0x1B4;
constexpr uint32_t kPlayerCutscene = 0xBA3;  // set by BeginCutScene, cleared by EndCutScene
constexpr uint32_t kPlayerZoom = 0x884;
// CZoom: active u8 +4, level count +0xC, magnification per level +0x10, first zoomed level +0x7C; the camera angle
// lerps to 90 / magnification
constexpr uint32_t kZoomOn = 0x04;
constexpr uint32_t kZoomLevels = 0x0C;
constexpr uint32_t kZoomMagnification = 0x10;
constexpr uint32_t kZoomFirstLevel = 0x7C;
constexpr uint32_t kMaxZoomLevels = (kZoomFirstLevel - kZoomMagnification) / 4;
constexpr double kStockFov = 1.5707963267948966;
constexpr double kWide = 16.0 / 9.0;
// COcean::Update sizes its projected grid from the stock m_fov, which spans about 1.2x the true view in tan space;
// the choppy grid edge needs that margin
constexpr double kOceanMargin = 1.25;

thread_local bool g_light_cameras = false;  // the shadow map loop's light cameras keep their own cone
thread_local double g_real_aspect = 0.0;    // while the camera update runs with a patched one

void WF(uint32_t addr, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(addr, raw);
}

bool GuestReadable(uint32_t addr, uint32_t size) {
    if (addr < 0x10000u || size == 0 || uint64_t(addr) + size > 0xC0000000u) return false;
    uint32_t first = addr & ~(mem::kPage - 1);
    uint32_t last = (addr + size - 1) & ~(mem::kPage - 1);
    for (uint32_t p = first;; p += mem::kPage) {
        if (!mem::IsCommitted(p)) return false;
        if (p == last) return true;
    }
}

// the game's angle with the user's angle in degrees applied; false in cutscenes
bool UserFov(double fov, double setting, double& out) {
    uint32_t player = fcip::LocalPlayer();
    if (GuestReadable(player, kPlayerCutscene + 1) && R8(player + kPlayerCutscene)) return false;
    if (!(fov > 0.0 && fov < 3.14)) return false;
    double user = setting * (3.14159265358979 / 180.0);
    out = 2.0 * std::atan(std::tan(fov * 0.5) * std::tan(user * 0.5));
    return true;
}

// the first zoom level's angle while the local player is zoomed, else 0
double FirstZoomFov() {
    uint32_t player = fcip::LocalPlayer();
    uint32_t zoom = GuestReadable(player, kPlayerZoom + 4) ? R32(player + kPlayerZoom) : 0;
    if ((zoom & 3u) != 0 || !GuestReadable(zoom, kZoomFirstLevel + 4) || !R8(zoom + kZoomOn)) return 0.0;
    uint32_t levels = R32(zoom + kZoomLevels);
    uint32_t level = R32(zoom + kZoomFirstLevel);
    if (levels == 0 || levels > kMaxZoomLevels || level >= levels) return 0.0;
    float magnification = GF(zoom + kZoomMagnification + level * 4);
    return std::isfinite(magnification) && magnification > 1.0f && magnification <= 64.0f
               ? kStockFov / magnification
               : 0.0;
}

// the camera's angle; above 90 the zoom levels keep their stock angles and the zoom blends into the user's angle
// between the first level and the unzoomed view
bool ViewFov(double fov, double& out) {
    double setting = fcip::Get().fov;
    if (!UserFov(fov, setting, out)) return false;
    double first = setting > 90.0 ? FirstZoomFov() : 0.0;
    if (first > 0.0 && fov < kStockFov) {
        double blend = std::clamp((fov - first) / (kStockFov - first), 0.0, 1.0);
        double user = setting * (kStockFov / 90.0);
        out = fov + (user - kStockFov) * blend;
    }
    return true;
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(CCamera_Update));
PPC_EXTERN_FUNC(GUEST_ORIG(PerspectiveFov));
PPC_EXTERN_FUNC(GUEST_ORIG(COcean_Update));

PPC_FUNC(GUEST(ShadowMaps_Update)) {
    g_light_cameras = true;
    fcip::DrawShadowMaps(ctx, base);
    g_light_cameras = false;
}

// m_fov becomes the true horizontal angle and m_aspect is swapped so m_fov / m_aspect is the true vertical one, which
// keeps the culling frustum exact on both axes; the projection then gets the real aspect back in PerspectiveFov
PPC_FUNC(GUEST(CCamera_Update)) {
    uint32_t cam = uint32_t(ctx.r[3]);
    bool cam_ok = GuestReadable(cam, kCameraAspect + 4);
    float fov = cam_ok && !g_light_cameras ? GF(cam + kCameraFov) : 0.0f;
    float aspect = fov > 0.0f ? GF(cam + kCameraAspect) : 0.0f;
    bool valid = aspect > 0.1f && aspect < 10.0f;
    bool screen = std::fabs(aspect - fcip::DisplayAspect()) < 0.01f * aspect;
    double wide = screen ? std::max(1.0, aspect / kWide) : 1.0;
    double h = 0.0;
    bool user = valid && ViewFov(fov, h);
    if (!user && !(valid && wide > 1.0 && fov < 3.14f)) {
        GUEST_ORIG(CCamera_Update)(ctx, base);
        return;
    }
    if (!user) h = 2.0 * std::atan(std::tan(fov * 0.5 / kWide) * kWide);
    h = 2.0 * std::atan(std::tan(h * 0.5) * wide);
    double v = 2.0 * std::atan(std::tan(h * 0.5) / aspect);
    float viewmodel = GF(cam + kCameraViewmodelFov);
    double vm = 0.0;
    bool user_vm = user && UserFov(viewmodel, fcip::Get().viewmodel_fov, vm);
    WF(cam + kCameraFov, float(h));
    WF(cam + kCameraAspect, float(h / v));
    if (user_vm) WF(cam + kCameraViewmodelFov, float(vm));
    g_real_aspect = aspect;
    GUEST_ORIG(CCamera_Update)(ctx, base);
    g_real_aspect = 0.0;
    WF(cam + kCameraFov, fov);
    WF(cam + kCameraAspect, aspect);
    WF(cam + kCameraViewmodelFov, viewmodel);
}

// D3DXMatrixPerspectiveFov(out, fovY f1, aspect f2, near f3, far f4)
PPC_FUNC(GUEST(PerspectiveFov)) {
    if (g_real_aspect > 0.0) ctx.f[2] = g_real_aspect;
    GUEST_ORIG(PerspectiveFov)(ctx, base);
}

// f3 the stock m_fov DrawOcean reads outside the camera call
PPC_FUNC(GUEST(COcean_Update)) {
    double h = 0.0;
    double wide = std::max(1.0, fcip::DisplayAspect() / kWide);
    double fov = ctx.f[3];
    if (ViewFov(fov, h)) ctx.f[3] = 2.0 * std::atan(std::tan(h * 0.5) * kOceanMargin * wide);
    else if (fov > 0.0 && fov < 3.14) ctx.f[3] = 2.0 * std::atan(std::tan(fov * 0.5) * wide);
    GUEST_ORIG(COcean_Update)(ctx, base);
}
