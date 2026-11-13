#include "d3d/dlss.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <filesystem>
#include <iterator>
#include <string>

#include <sl.h>
#include <sl_security.h>

#include "core/log.h"

namespace dlss {

namespace {

// Frame Generation needs Reflex and PCL loaded beside it
constexpr sl::Feature kFeatures[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
constexpr char kProjectId[] = "3afb89d7-853e-41e7-b84e-83a34cef2d84";

PFun_slInit* g_init = nullptr;
PFun_slShutdown* g_shutdown = nullptr;
PFun_slSetD3DDevice* g_set_device = nullptr;
PFun_slIsFeatureSupported* g_is_supported = nullptr;

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
                 Find(dll, "slSetD3DDevice", g_set_device) && Find(dll, "slIsFeatureSupported", g_is_supported);
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
    g_sr = sr == sl::Result::eOk;
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

bool SuperResolution() { return g_sr; }
bool FrameGeneration() { return g_fg; }

}  // namespace dlss
