// The field of view: the camera takes the user's true horizontal angle, zoom kept in tan space against the stock 90,
// and above 16:9 the view keeps its 16:9 height and widens. The weapon and arms get their own projection from the
// camera's second angle, scaled the same way by the viewmodel setting. Cutscenes keep their own angles.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/aspect.h"
#include "common/input.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// CCamera: m_fov horizontal radians at +0x3C, m_aspect at +0x1B4; Update builds the projection with fovY = m_fov /
// m_aspect, so stock 90 shows about 80 horizontal at 16:9
constexpr uint32_t kCameraFov = 0x3C;
constexpr uint32_t kCameraViewmodelFov = 0x40;  // vertical radians, the near projection at +0x2BC
constexpr uint32_t kCameraAspect = 0x1B4;
constexpr uint32_t kPlayerCutscene = 0xBA3;  // set by BeginCutScene, cleared by EndCutScene
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

// the game's angle with the user's angle in degrees applied; false in cutscenes
bool UserFov(double fov, double setting, double& out) {
    uint32_t player = fcip::LocalPlayer();
    if (player >= 0x10000u && player < 0xC0000000u && R8(player + kPlayerCutscene)) return false;
    if (!(fov > 0.0 && fov < 3.14)) return false;
    double user = setting * (3.14159265358979 / 180.0);
    out = 2.0 * std::atan(std::tan(fov * 0.5) * std::tan(user * 0.5));
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
    float fov = cam && !g_light_cameras ? GF(cam + kCameraFov) : 0.0f;
    float aspect = fov > 0.0f ? GF(cam + kCameraAspect) : 0.0f;
    bool valid = aspect > 0.1f && aspect < 10.0f;
    bool screen = std::fabs(aspect - fcip::DisplayAspect()) < 0.01f * aspect;
    double wide = screen ? std::max(1.0, aspect / kWide) : 1.0;
    double h = 0.0;
    bool user = valid && UserFov(fov, fcip::Get().fov, h);
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
    if (UserFov(fov, fcip::Get().fov, h)) ctx.f[3] = 2.0 * std::atan(std::tan(h * 0.5) * kOceanMargin * wide);
    else if (fov > 0.0 && fov < 3.14) ctx.f[3] = 2.0 * std::atan(std::tan(fov * 0.5) * wide);
    GUEST_ORIG(COcean_Update)(ctx, base);
}
