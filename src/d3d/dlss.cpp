#include "d3d/dlss.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include "core/log.h"

namespace dlss {

namespace {

// Frame Generation needs Reflex and PCL loaded beside it
constexpr sl::Feature kFeatures[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
constexpr char kProjectId[] = "3afb89d7-853e-41e7-b84e-83a34cef2d84";
// the DLLs from NVIDIA's Streamline release each feature needs beside the exe
constexpr const wchar_t* kUpscalingDlls[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"nvngx_dlss.dll"};
constexpr const wchar_t* kGenerationDlls[] = {L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll"};
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
PFun_slDLSSGSetOptions* g_fg_options = nullptr;
PFun_slDLSSGGetState* g_fg_state = nullptr;
PFun_slReflexSetOptions* g_reflex_options = nullptr;
PFun_slReflexSleep* g_reflex_sleep = nullptr;
PFun_slPCLSetMarker* g_marker = nullptr;

bool g_loaded = false;
bool g_sr_dlls = false;
bool g_fg_dlls = false;
bool g_sr = false;
bool g_fg = false;
uint32_t g_fg_max = 1;  // multiplier
bool g_fg_dynamic = false;
std::mutex g_token_lock;  // tokens are asked for on the title and the render thread
sl::DLSSGMode g_fg_mode = sl::DLSSGMode::eOff;
uint32_t g_fg_frames = 0;  // generated per rendered frame

void StreamlineLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError) LOGE("Streamline: %s", msg);
}

template <size_t N>
bool Beside(const std::filesystem::path& dir, const wchar_t* const (&names)[N]) {
    for (const wchar_t* name : names) {
        if (!std::filesystem::exists(dir / name)) return false;
    }
    return true;
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

sl::FrameToken* Token(uint32_t frame) {
    std::lock_guard lock(g_token_lock);
    sl::FrameToken* token = nullptr;
    return g_new_token(token, &frame) == sl::Result::eOk ? token : nullptr;
}

template <typename F>
bool Feature(sl::Feature feature, const char* name, F*& fn) {
    void* p = nullptr;
    bool ok = g_feature_function(feature, name, p) == sl::Result::eOk && p;
    fn = reinterpret_cast<F*>(p);
    return ok;
}

// Reflex, PCL and DLSS-G's entry points, then the most frames this GPU generates
bool LoadFrameGeneration() {
    bool found = Feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_fg_options) &&
                 Feature(sl::kFeatureDLSS_G, "slDLSSGGetState", g_fg_state) &&
                 Feature(sl::kFeatureReflex, "slReflexSetOptions", g_reflex_options) &&
                 Feature(sl::kFeatureReflex, "slReflexSleep", g_reflex_sleep) &&
                 Feature(sl::kFeaturePCL, "slPCLSetMarker", g_marker);
    if (!found) return false;
    sl::ReflexOptions reflex;
    g_reflex_options(reflex);
    sl::DLSSGState state;
    if (g_fg_state(sl::ViewportHandle(0u), state, nullptr) != sl::Result::eOk) return false;
    g_fg_max = state.numFramesToGenerateMax + 1;
    g_fg_dynamic = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    return true;
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
    g_sr_dlls = Beside(dir, kUpscalingDlls);
    g_fg_dlls = g_sr_dlls && Beside(dir, kGenerationDlls);
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
    g_fg = fg == sl::Result::eOk && LoadFrameGeneration();
    LOGR("DLSS: Super Resolution %s (result %d), Frame Generation %s (result %d)", g_sr ? "available" : "unavailable",
         int(sr), g_fg ? "available" : "unavailable", int(fg));
    if (g_fg) LOGR("DLSS: Frame Generation up to %ux, dynamic %s", g_fg_max, g_fg_dynamic ? "yes" : "no");
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

Status UpscalingStatus() {
    if (g_sr) return Status::Ready;
    return g_sr_dlls ? Status::NotSupported : Status::MissingDll;
}

Status GenerationStatus() {
    if (g_fg) return Status::Ready;
    return g_fg_dlls ? Status::NotSupported : Status::MissingDll;
}

const char* StatusNote(Status s) {
    switch (s) {
        case Status::MissingDll: return " (MISSING DLL)";
        case Status::NotSupported: return " (NOT SUPPORTED)";
        default: return "";
    }
}

bool SuperResolution() { return g_sr; }
bool FrameGeneration() { return g_fg; }
uint32_t MaxMultiplier() { return g_fg ? g_fg_max : 1; }
bool DynamicFrameGeneration() { return g_fg && g_fg_dynamic; }

void Mark(Marker m, uint32_t frame) {
    if (!g_fg) return;
    if (sl::FrameToken* token = Token(frame)) g_marker(sl::PCLMarker(m), *token);
}

void Sleep(uint32_t frame) {
    if (!g_fg) return;
    if (sl::FrameToken* token = Token(frame)) g_reflex_sleep(*token);
}

void Generate(uint32_t frame, uint32_t setting, bool gameplay, const uint32_t rect[4], uint32_t w, uint32_t h) {
    if (!g_fg) return;
    sl::DLSSGMode mode = sl::DLSSGMode::eOff;
    uint32_t frames = 1;
    if (gameplay && setting == kGenerationDynamic && g_fg_dynamic) mode = sl::DLSSGMode::eDynamic;
    if (gameplay && setting >= 2 && setting <= g_fg_max) {
        mode = sl::DLSSGMode::eOn;
        frames = setting - 1;
    }
    sl::ViewportHandle viewport(0u);
    // the rest of the back buffer, the bars around a narrower frame, is copied as it is
    bool whole = rect[0] == 0 && rect[1] == 0 && rect[2] == w && rect[3] == h;
    sl::FrameToken* token = mode != sl::DLSSGMode::eOff && !whole ? Token(frame) : nullptr;
    if (token) {
        sl::Extent part{rect[1], rect[0], rect[2], rect[3]};
        sl::ResourceTag tag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &part);
        Ok(g_set_tags(*token, viewport, &tag, 1, nullptr), "back buffer tag");
    }
    if (mode == g_fg_mode && frames == g_fg_frames) return;
    g_fg_mode = mode;
    g_fg_frames = frames;
    // Reflex must be on while frames are generated
    sl::ReflexOptions reflex;
    reflex.mode = mode == sl::DLSSGMode::eOff ? sl::ReflexMode::eOff : sl::ReflexMode::eLowLatency;
    Ok(g_reflex_options(reflex), "slReflexSetOptions");
    sl::DLSSGOptions o;
    o.mode = mode;
    o.numFramesToGenerate = frames;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    Ok(g_fg_options(viewport, o), "slDLSSGSetOptions");
}

bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_sr) return false;
    sl::FrameToken* token = Token(f.index);
    if (!token) return false;
    sl::ViewportHandle viewport(0u);
    sl::DLSSOptions o;
    o.mode = ModeFor(f.color.h, f.output.h);
    o.outputWidth = f.output.w;
    o.outputHeight = f.output.h;
    o.colorBuffersHDR = sl::Boolean::eFalse;
    o.useAutoExposure = sl::Boolean::eTrue;
    o.dlaaPreset = sl::DLSSPreset::ePresetM;
    o.qualityPreset = sl::DLSSPreset::ePresetM;
    o.balancedPreset = sl::DLSSPreset::ePresetM;
    o.performancePreset = sl::DLSSPreset::ePresetM;
    o.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
    o.ultraQualityPreset = sl::DLSSPreset::ePresetM;
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
