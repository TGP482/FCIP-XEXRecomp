#include "d3d/dlss.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_security.h>

#include "core/log.h"

namespace dlss {

namespace {

// Frame Generation needs Reflex and PCL loaded beside it
constexpr sl::Feature kFeatures[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
constexpr char kProjectId[] = "3afb89d7-853e-41e7-b84e-83a34cef2d84";
// kModes in order, as DLSS names them
constexpr sl::DLSSMode kModeEnums[] = {sl::DLSSMode::eUltraPerformance, sl::DLSSMode::eMaxPerformance,
                                       sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eDLAA};
static_assert(std::size(kModeEnums) == std::size(kModes));

PFun_slInit* g_init = nullptr;
PFun_slShutdown* g_shutdown = nullptr;
PFun_slSetD3DDevice* g_set_device = nullptr;
PFun_slIsFeatureSupported* g_is_supported = nullptr;
PFun_slGetFeatureFunction* g_feature_function = nullptr;
PFun_slGetNewFrameToken* g_new_token = nullptr;
PFun_slSetConstants* g_set_constants = nullptr;
PFun_slSetTagForFrame* g_set_tags = nullptr;
PFun_slEvaluateFeature* g_evaluate = nullptr;
PFun_slUpgradeInterface* g_upgrade = nullptr;
PFun_slDLSSSetOptions* g_dlss_options = nullptr;

bool g_loaded = false;
bool g_sr = false;
bool g_fg = false;

void StreamlineLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError) LOGE("Streamline: %s", msg);
}

template <typename F>
bool Find(HMODULE dll, const char* name, F*& fn) {
    fn = reinterpret_cast<F*>(GetProcAddress(dll, name));
    return fn != nullptr;
}

sl::Result Supported(sl::Feature feature, LUID luid) {
    sl::AdapterInfo a;
    a.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    a.deviceLUIDSizeInBytes = sizeof(luid);
    return g_is_supported(feature, a);
}

// the first failure of each step, so a frame that keeps failing logs once
bool Ok(sl::Result r, const char* step) {
    static bool logged = false;
    if (r == sl::Result::eOk) return true;
    if (!logged) LOGE("DLSS: %s failed (result %d)", step, int(r));
    logged = true;
    return false;
}

// the mode whose render resolution lies nearest the scene's
sl::DLSSMode ModeFor(uint32_t in_h, uint32_t out_h) {
    float percent = 100.0f * float(in_h) / float(out_h);
    size_t best = 0;
    for (size_t i = 1; i < std::size(kModes); i++) {
        if (std::fabs(float(kModes[i].percent) - percent) < std::fabs(float(kModes[best].percent) - percent)) best = i;
    }
    return kModeEnums[best];
}

sl::Resource Resource(const Image& i) {
    sl::Resource r(sl::ResourceType::eTex2d, i.res, i.state);
    r.width = i.w;
    r.height = i.h;
    if (i.format) r.nativeFormat = i.format;
    return r;
}

void Matrix(const float m[16], sl::float4x4& out) { std::memcpy(&out, m, 16 * sizeof(float)); }

// the camera as DLSS wants it; the title's projection is right handed, its pass flips depth (823B0580)
void FillConstants(const Frame& f, sl::Constants& c) {
    const float* p = f.camera.proj;
    float view_to_clip[16];
    std::memcpy(view_to_clip, p, sizeof(view_to_clip));
    for (int r = 0; r < 4; r++) view_to_clip[r * 4 + 2] = p[r * 4 + 3] - p[r * 4 + 2];
    float clip_to_view[16];
    float prev_to_clip[16];
    float world[16];
    gpu::InvertMatrix(view_to_clip, clip_to_view);
    gpu::InvertMatrix(f.clip_to_prev, prev_to_clip);
    gpu::InvertMatrix(f.camera.view, world);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Matrix(view_to_clip, c.cameraViewToClip);
    Matrix(clip_to_view, c.clipToCameraView);
    Matrix(identity, c.clipToLensClip);
    Matrix(f.clip_to_prev, c.clipToPrevClip);
    Matrix(prev_to_clip, c.prevClipToClip);
    c.jitterOffset = {f.jitter[0], f.jitter[1]};
    c.mvecScale = {1, 1};
    c.cameraPinholeOffset = {0, 0};
    c.cameraRight = {world[0], world[1], world[2]};
    c.cameraUp = {world[4], world[5], world[6]};
    c.cameraFwd = {-world[8], -world[9], -world[10]};
    c.cameraPos = {world[12], world[13], world[14]};
    c.cameraNear = p[14] / p[10];
    c.cameraFar = p[14] / (p[10] + 1);
    c.cameraFOV = 2 * std::atan(1 / p[5]);
    c.cameraAspectRatio = p[5] / p[0];
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

}  // namespace

void Load() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    std::wstring interposer = (dir / L"sl.interposer.dll").wstring();
    if (!std::filesystem::exists(interposer)) {
        LOGR("DLSS: no sl.interposer.dll beside the exe");
        return;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        LOGE("DLSS: sl.interposer.dll is not signed by NVIDIA");
        return;
    }
    HMODULE dll = LoadLibraryW(interposer.c_str());
    bool found = dll && Find(dll, "slInit", g_init) && Find(dll, "slShutdown", g_shutdown) &&
                 Find(dll, "slSetD3DDevice", g_set_device) && Find(dll, "slIsFeatureSupported", g_is_supported) &&
                 Find(dll, "slGetFeatureFunction", g_feature_function) &&
                 Find(dll, "slGetNewFrameToken", g_new_token) && Find(dll, "slSetConstants", g_set_constants) &&
                 Find(dll, "slSetTagForFrame", g_set_tags) && Find(dll, "slEvaluateFeature", g_evaluate) &&
                 Find(dll, "slUpgradeInterface", g_upgrade);
    if (!found) {
        LOGE("DLSS: sl.interposer.dll could not be loaded");
        return;
    }
    std::wstring plugins = dir.wstring();
    const wchar_t* paths[] = {plugins.c_str()};
    sl::Preferences p;
    p.logMessageCallback = StreamlineLog;
    p.pathsToPlugins = paths;
    p.numPathsToPlugins = 1;
    p.flags |= sl::PreferenceFlags::eUseManualHooking;
    p.featuresToLoad = kFeatures;
    p.numFeaturesToLoad = uint32_t(std::size(kFeatures));
    p.engine = sl::EngineType::eCustom;
    p.engineVersion = "1";
    p.projectId = kProjectId;
    if (sl::Result r = g_init(p, sl::kSDKVersion); r != sl::Result::eOk) {
        LOGE("DLSS: Streamline did not start (result %d)", int(r));
        return;
    }
    g_loaded = true;
}

void SetDevice(ID3D12Device* dev) {
    if (!g_loaded) return;
    if (sl::Result r = g_set_device(dev); r != sl::Result::eOk) {
        LOGE("DLSS: Streamline did not take the device (result %d)", int(r));
        return;
    }
    LUID luid = dev->GetAdapterLuid();
    sl::Result sr = Supported(sl::kFeatureDLSS, luid);
    sl::Result fg = Supported(sl::kFeatureDLSS_G, luid);
    void* options = nullptr;
    if (sr == sl::Result::eOk) sr = g_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", options);
    g_dlss_options = reinterpret_cast<PFun_slDLSSSetOptions*>(options);
    g_sr = sr == sl::Result::eOk && g_dlss_options;
    g_fg = fg == sl::Result::eOk;
    LOGR("DLSS: Super Resolution %s (result %d), Frame Generation %s (result %d)", g_sr ? "available" : "unavailable",
         int(sr), g_fg ? "available" : "unavailable", int(fg));
}

void Shutdown() {
    if (!g_loaded) return;
    g_shutdown();
    g_loaded = false;
    g_sr = false;
    g_fg = false;
}

void UpgradeFactory(void** factory) {
    if (!g_sr && !g_fg) return;
    sl::Result r = g_upgrade(factory);
    if (r != sl::Result::eOk) LOGE("DLSS: no Streamline factory (result %d)", int(r));
}

bool SuperResolution() { return g_sr; }
bool FrameGeneration() { return g_fg; }

bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_sr) return false;
    sl::FrameToken* token = nullptr;
    if (!Ok(g_new_token(token, &f.index), "slGetNewFrameToken")) return false;
    sl::ViewportHandle viewport(0u);
    sl::DLSSOptions o;
    o.mode = ModeFor(f.color.h, f.output.h);
    o.outputWidth = f.output.w;
    o.outputHeight = f.output.h;
    o.colorBuffersHDR = sl::Boolean::eFalse;
    o.useAutoExposure = sl::Boolean::eTrue;
    if (!Ok(g_dlss_options(viewport, o), "slDLSSSetOptions")) return false;
    sl::Constants c;
    FillConstants(f, c);
    if (!Ok(g_set_constants(c, *token, viewport), "slSetConstants")) return false;
    sl::Resource color = Resource(f.color);
    sl::Resource depth = Resource(f.depth);
    sl::Resource motion = Resource(f.motion);
    sl::Resource output = Resource(f.output);
    sl::Extent in{0, 0, f.color.w, f.color.h};
    sl::Extent out{0, 0, f.output.w, f.output.h};
    sl::ResourceTag tags[] = {
        {&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &in},
        {&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &out},
        {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &in},
        {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &in},
    };
    auto* list = reinterpret_cast<sl::CommandBuffer*>(cl);
    if (!Ok(g_set_tags(*token, viewport, tags, uint32_t(std::size(tags)), list), "slSetTagForFrame")) return false;
    const sl::BaseStructure* inputs[] = {&viewport};
    return Ok(g_evaluate(sl::kFeatureDLSS, *token, inputs, uint32_t(std::size(inputs)), list), "slEvaluateFeature");
}

}  // namespace dlss
