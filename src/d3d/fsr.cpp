#include "d3d/fsr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_5.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <utility>

#include <api/include/dx12/ffx_api_dx12.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <upscalers/include/ffx_upscale.h>

#include "core/log.h"

namespace fsr {

namespace {

// the loader finds the effect DLLs in its own folder
constexpr const wchar_t* kLoaderDll = L"amd_fidelityfx_loader_dx12.dll";
constexpr const wchar_t* kUpscalerDll = L"amd_fidelityfx_upscaler_dx12.dll";
constexpr const wchar_t* kGenerationDll = L"amd_fidelityfx_framegeneration_dx12.dll";

PfnFfxCreateContext g_create = nullptr;
PfnFfxDestroyContext g_destroy = nullptr;
PfnFfxConfigure g_configure = nullptr;
PfnFfxQuery g_query = nullptr;
PfnFfxDispatch g_dispatch = nullptr;

bool g_loaded = false;
bool g_sr_dlls = false;
bool g_fg_dlls = false;
bool g_sr = false;
bool g_fg = false;
ID3D12Device* g_device = nullptr;

ffxContext g_upscale = nullptr;
uint32_t g_upscale_w = 0;  // the output size it was made for
uint32_t g_upscale_h = 0;
ffxContext g_swap = nullptr;  // frame generation's swap chain
ffxContext g_generate = nullptr;
uint32_t g_generate_w = 0;  // the back buffer it was made for
uint32_t g_generate_h = 0;
uint32_t g_generate_format = 0;
uint64_t g_frame_time = 0;  // ns, the last Upscale
float g_delta_ms = 0;       // from the Upscale before
std::atomic<bool> g_generating{false};  // the last frame set up generated one between it and the one before

// the first errors and warnings only: one that repeats each frame would flood the log
void Message(uint32_t type, const wchar_t* msg) {
    constexpr int kMaxMessages = 32;
    static std::atomic<int> count{0};
    if (count++ >= kMaxMessages) return;
    if (type == FFX_API_MESSAGE_TYPE_ERROR) LOGE("FSR: %ls", msg);
    else LOGR("FSR: %ls", msg);
}

template <typename F>
bool Find(HMODULE dll, const char* name, F& fn) {
    fn = reinterpret_cast<F>(GetProcAddress(dll, name));
    return fn != nullptr;
}

// whether this device runs an effect: the loader names its versions for it
bool Available(uint64_t type) {
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = type;
    q.device = g_device;
    uint64_t count = 0;
    q.outputCount = &count;
    return g_query(nullptr, &q.header) == FFX_API_RETURN_OK && count;
}

void LogVersion(ffxContext* ctx, const char* what) {
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_query(ctx, &q.header) == FFX_API_RETURN_OK && q.versionName) LOGR("FSR: %s %s", what, q.versionName);
}

void Destroy(ffxContext& ctx) {
    if (ctx) g_destroy(&ctx, nullptr);
    ctx = nullptr;
}

// the upscaler for an output size, made again when it changes
bool EnsureUpscale(uint32_t w, uint32_t h) {
    if (g_upscale && g_upscale_w == w && g_upscale_h == h) return true;
    Destroy(g_upscale);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &version.header;
    d.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {w, h};
    d.fpMessage = Message;
    if (ffxReturnCode_t r = g_create(&g_upscale, &d.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no upscaler for %ux%u (result %u)", w, h, r);
        g_upscale = nullptr;
        return false;
    }
    g_upscale_w = w;
    g_upscale_h = h;
    LogVersion(&g_upscale, "upscaling");
    return true;
}

// frame generation for a back buffer, made again when it changes
bool EnsureGeneration(const Generation& g) {
    bool same = g_generate_w == g.w && g_generate_h == g.h && g_generate_format == g.format;
    if (g_generate && same) return true;
    Destroy(g_generate);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.header.pNext = &backend.header;
    hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(g.format));
    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.header.pNext = &hudless.header;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &version.header;
    d.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (g.hdr) d.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    d.displaySize = {g.w, g.h};
    d.maxRenderSize = {g.w, g.h};
    d.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(g.format));
    if (ffxReturnCode_t r = g_create(&g_generate, &d.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no frame generation for %ux%u (result %u)", g.w, g.h, r);
        g_generate = nullptr;
        return false;
    }
    g_generate_w = g.w;
    g_generate_h = g.h;
    g_generate_format = g.format;
    LogVersion(&g_generate, "frame generation");
    return true;
}

// generated frames made on the swap chain's present, from the context Configure passed
ffxReturnCode_t GenerateFrames(ffxDispatchDescFrameGeneration* params, void* ctx) {
    return g_dispatch(static_cast<ffxContext*>(ctx), &params->header);
}

FfxApiResource Resource(const Image& i, uint32_t state) {
    FfxApiResource r = ffxApiGetResourceDX12(i.res, state);
    if (i.format) r.description.format = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(i.format));
    return r;
}

// the camera as FSR wants it; the title's projection is right handed, its pass flips depth (823B0580)
struct Camera {
    float near_plane = 0;
    float far_plane = 0;
    float fov_y = 0;
    float world[16] = {};  // view to world
};

Camera CameraOf(const Frame& f) {
    const float* p = f.camera.proj;
    Camera c;
    c.near_plane = p[14] / p[10];
    c.far_plane = p[14] / (p[10] + 1);
    c.fov_y = 2 * std::atan(1 / p[5]);
    gpu::InvertMatrix(f.camera.view, c.world);
    return c;
}

}  // namespace

void Load() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exe).parent_path() / L"FSR";
    g_sr_dlls = std::filesystem::exists(dir / kLoaderDll) && std::filesystem::exists(dir / kUpscalerDll);
    g_fg_dlls = g_sr_dlls && std::filesystem::exists(dir / kGenerationDll);
    if (!g_sr_dlls) {
        LOGR("FSR: no AMD FidelityFX DLLs in the FSR folder");
        return;
    }
    // its own folder first for what it loads
    HMODULE dll = LoadLibraryExW((dir / kLoaderDll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    g_loaded = dll && Find(dll, "ffxCreateContext", g_create) && Find(dll, "ffxDestroyContext", g_destroy) &&
               Find(dll, "ffxConfigure", g_configure) && Find(dll, "ffxQuery", g_query) &&
               Find(dll, "ffxDispatch", g_dispatch);
    if (!g_loaded) LOGE("FSR: amd_fidelityfx_loader_dx12.dll could not be loaded");
}

void SetDevice(ID3D12Device* dev) {
    if (!g_loaded) return;
    g_device = dev;
    g_sr = Available(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
    g_fg = g_fg_dlls && Available(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    LOGR("FSR: upscaling %s, frame generation %s", g_sr ? "available" : "unavailable",
         g_fg ? "available" : "unavailable");
}

void Shutdown() {
    if (!g_loaded) return;
    Destroy(g_generate);
    Destroy(g_upscale);
    g_sr = false;
    g_fg = false;
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

bool Upscaling() { return g_sr; }
bool FrameGeneration() { return g_fg; }
bool Generating() { return g_generating; }

bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_sr || !EnsureUpscale(f.output.w, f.output.h)) return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    g_delta_ms = g_frame_time ? float(ns - g_frame_time) / 1e6f : 0.0f;
    g_frame_time = ns;
    Camera c = CameraOf(f);
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = Resource(f.color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = Resource(f.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = Resource(f.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = Resource(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {float(f.color.w), float(f.color.h)};
    d.renderSize = {f.color.w, f.color.h};
    d.upscaleSize = {f.output.w, f.output.h};
    d.frameTimeDelta = g_delta_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = c.near_plane;
    d.cameraFar = c.far_plane;
    d.cameraFovAngleVertical = c.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = g_dispatch(&g_upscale, &d.header);
    static bool logged = false;
    if (r != FFX_API_RETURN_OK && !std::exchange(logged, true)) LOGE("FSR: upscale failed (result %u)", r);
    return r == FFX_API_RETURN_OK;
}

bool CreateSwapChain(IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND__* hwnd, const DXGI_SWAP_CHAIN_DESC1& d,
                     IDXGISwapChain4** out) {
    if (!g_fg) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = d;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    c.header.pNext = &version.header;
    c.swapchain = out;
    c.hwnd = hwnd;
    c.desc = &desc;
    c.dxgiFactory = factory;
    c.gameQueue = queue;
    if (ffxReturnCode_t r = g_create(&g_swap, &c.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no frame generation swap chain (result %u)", r);
        g_swap = nullptr;
        return false;
    }
    LogVersion(&g_swap, "frame generation swap chain");
    return true;
}

void DestroySwapChain(IDXGISwapChain4* swap) {
    g_generating = false;
    if (!g_swap) return;
    // generation off first: it waits for the swap chain's work on its resources
    if (g_generate) {
        ffxConfigureDescFrameGeneration off{};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain = swap;
        g_configure(&g_generate, &off.header);
    }
    Destroy(g_generate);
    Destroy(g_swap);
}

void Configure(const Generation& g) {
    if (!g_swap || !EnsureGeneration(g)) return;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g.swap;
    c.frameGenerationCallback = GenerateFrames;
    c.frameGenerationCallbackUserContext = &g_generate;
    c.frameGenerationEnabled = g.enabled;
    c.generationRect = {int32_t(g.rect[0]), int32_t(g.rect[1]), int32_t(g.rect[2]), int32_t(g.rect[3])};
    c.frameID = g.index;
    if (g.hudless) c.HUDLessColor = ffxApiGetResourceDX12(g.hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    static bool logged = false;
    ffxReturnCode_t r = g_configure(&g_generate, &c.header);
    if (r != FFX_API_RETURN_OK && !std::exchange(logged, true)) LOGE("FSR: frame generation setup failed (result %u)", r);
    g_generating = r == FFX_API_RETURN_OK && g.enabled;
}

void Prepare(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_generate) return;
    Camera c = CameraOf(f);
    ffxDispatchDescFrameGenerationPrepareV2 d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = f.index;
    d.commandList = cl;
    d.renderSize = {f.color.w, f.color.h};
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {float(f.color.w), float(f.color.h)};
    d.frameTimeDelta = g_delta_ms;
    d.reset = f.reset;
    d.cameraNear = c.near_plane;
    d.cameraFar = c.far_plane;
    d.cameraFovAngleVertical = c.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.depth = Resource(f.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = Resource(f.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    const float* w = c.world;
    for (int i = 0; i < 3; i++) {
        d.cameraRight[i] = w[i];
        d.cameraUp[i] = w[4 + i];
        d.cameraForward[i] = -w[8 + i];
        d.cameraPosition[i] = w[12 + i];
    }
    static bool logged = false;
    ffxReturnCode_t r = g_dispatch(&g_generate, &d.header);
    if (r != FFX_API_RETURN_OK && !std::exchange(logged, true)) LOGE("FSR: generation inputs failed (result %u)", r);
}

}  // namespace fsr
