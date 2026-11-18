#include "gpu/backend.h"
#include "gpu/compiler.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/texturedumper.h"
#include "gpu/xenos_format.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/fsr.h"
#include "d3d/motion.h"
#include "d3d/occlusion.h"
#include "d3d/readback.h"
#include "d3d/cas_shader.h"
#include "d3d/smaa_shader.h"
#include "d3d/smaa_textures.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "core/diag.h"
#include "core/log.h"
#include "imgui.h"
#include "kernel/memory.h"

namespace gpu {

std::atomic<bool> g_ray_tracing{false};

bool RayTracing() { return g_ray_tracing; }

// a translation of the title's microcode for one set of options
struct Variant {
    ShaderInfo info;
    std::vector<uint8_t> dxil;
    uint64_t key = 0;  // of the DXIL, so a cached pipeline of other DXIL never matches
    std::atomic<bool> building{false};  // compiling on a build thread; dxil and key are read once it is done
};

struct Shader {
    ShaderStage stage;
    std::vector<uint32_t> code;
    uint64_t hash = 0;  // of code
    ShaderInfo info;  // default options: what the title's Direct3D links and fetches by
    std::unordered_map<uint64_t, std::unique_ptr<Variant>> variants;
    ShaderOptions last_opt;  // the last variant asked for, found again without a hash
    Variant* last = nullptr;
    fcip::FirstFetch first_fetch;  // pixel: what an alpha tested shadow caster's test reads
    bool material = false;         // pixel: tints its texture by fcip::kMaterialColour
    bool ambient = false;          // reads the ambient its draw is lit by, fcip::kPsAmbient or fcip::kVsAmbient
};

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kFrames = 2;
// diag: GPU time by kind of work, a timestamp where the kind changes
constexpr uint32_t kStamps = 4096;
enum : uint8_t { kStampOther, kStampTraced, kStampShadowMap, kStampScene, kStampTracedAlpha, kStampEnd };
constexpr uint64_t kUploadSize = 96ull << 20;  // per frame
constexpr uint32_t kViewsPerFrame = 1u << 18;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr uint32_t kQueryCount = kQuerySlots;
constexpr uint32_t kVsTextures = 16;
constexpr uint32_t kPsTextures = 32;
constexpr uint32_t kTextureViews = 16384;  // made views kept for copying, started over when full
constexpr uint32_t kConstBytes = 256 * 16;
constexpr uint32_t kCbAlign = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t kTileRows = 16;  // samples per EDRAM tile column
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
// HDR10 output: back buffers in PQ over BT.2020, the frame and the panel drawn into their own targets first; SDR white
// and peak in nits when Windows can't tell
constexpr DXGI_FORMAT kHdr10Format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kOutFrameFormat = DXGI_FORMAT_R16G16B16A16_UNORM;  // the frame before PQ
constexpr int kOutFrame = 0;
constexpr int kOutPanel = 1;
constexpr int kOutTargets = 2;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;
constexpr float kMaxPeakNits = 4000.0f;  // brighter is a display without calibration reporting PQ's 10000
constexpr float kSdrWhiteUnit = 0.08f;   // Windows' SDR white level: 1000 is 80 nits
constexpr uint32_t kNullRtv = kRtvCount + kFrames;  // a target slot between bound ones, written nowhere
constexpr DXGI_FORMAT kNullRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kOutRtv = kNullRtv + 1;  // the HDR10 output's frame and panel targets
constexpr int kSmaaTargets = 3;
constexpr uint32_t kSmaaRtv = kOutRtv + kOutTargets;  // SMAA's edges, blend weights and blended frame
constexpr int kAoTargets = 2;
constexpr uint32_t kAoRtv = kSmaaRtv + kSmaaTargets;  // ambient occlusion's raw pass and first blur
constexpr uint32_t kMotionRtv = kAoRtv + kAoTargets;  // motion vectors as the draws wrote them, then completed
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr uint32_t kHudlessRtv = kMotionRtv + 2;  // frame generation's two back buffers without the HUD
constexpr uint32_t kAlbedoRtv = kHudlessRtv + 2;   // global illumination's albedo, then its traced light and first blur
constexpr uint32_t kGiRtv = kAlbedoRtv + 1;
constexpr int kGiTargets = 2;
constexpr uint32_t kFogRtv = kGiRtv + kGiTargets;  // volumetric fog's march and first blur
constexpr int kFogTargets = 2;
constexpr uint32_t kCloudRtv = kFogRtv + kFogTargets;  // volumetric clouds' march
constexpr uint32_t kCausticsRtv = kCloudRtv + 1;       // the caustics' light
constexpr uint32_t kRtvSlots = kCausticsRtv + 1;
constexpr DXGI_FORMAT kAlbedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kGiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// a full float: the water's draws fade the light by the floor's distance less theirs, which half floats step through
constexpr DXGI_FORMAT kCausticsFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr float kNoMotion = 32768;  // the drawn motion's clear: no draw wrote the pixel
constexpr uint64_t kSceneTarget = 1ull << 62;  // targets_ key bit of a target at the scene's scale, before FSR
constexpr DXGI_FORMAT kUpscaledFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum : uint32_t {
    kRootVsConsts,
    kRootPsConsts,
    kRootFlow,
    kRootDraw,
    kRootFetch,
    kRootVsViews,
    kRootPsViews,
    kRootVsSamplers,
    kRootPsSamplers,
    kRootPrevConsts,  // a motion draw's vertex constants last frame
    kRootMotion,
    kRootHeap,  // the whole view heap, for ray traced shadows' alpha tests; only with ray tracing
    kRootCount,
};

uint32_t Bits(uint32_t v, uint32_t at, uint32_t n) { return v >> at & ((1u << n) - 1); }
int32_t SignedBits(uint32_t v, uint32_t at, uint32_t n) { return int32_t(Bits(v, at, n) << (32 - n)) >> (32 - n); }

uint32_t Be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

// GPU addresses are physical, read through the 4 KB page window
const uint8_t* Guest(uint32_t address) { return mem::Base() + mem::VirtualFromPhysical(address); }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// the DXGI output on monitor mon
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor == mon) return true;
        }
    }
    return false;
}

// Windows' SDR content brightness on monitor mon, in nits
float SdrWhiteNits(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (!GetMonitorInfoW(mon, &info)) return kDefaultPaperNits;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kDefaultPaperNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                      nullptr);
    if (queried != ERROR_SUCCESS) return kDefaultPaperNits;
    for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) break;
        return float(white.SDRWhiteLevel) * kSdrWhiteUnit;
    }
    return kDefaultPaperNits;
}

D3D12_RESOURCE_DESC TextureDesc(DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t depth = 1, uint32_t mips = 1,
                                bool volume = false) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = std::max(w, 1u);
    d.Height = std::max(h, 1u);
    d.DepthOrArraySize = UINT16(std::max(depth, 1u));
    d.MipLevels = UINT16(std::max(mips, 1u));
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    return d;
}

// render target formats (RB_COLOR_INFO), held in the closest host format with at least their range
DXGI_FORMAT ColorFormat(uint32_t f) {
    switch (f) {
        case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 1: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case 2:
        case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 4:
        case 6: return DXGI_FORMAT_R16G16_FLOAT;
        case 14: return DXGI_FORMAT_R32_FLOAT;
        case 15: return DXGI_FORMAT_R32G32_FLOAT;
        default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}
DXGI_FORMAT ColorResourceFormat(uint32_t f) { return f <= 1 ? DXGI_FORMAT_R8G8B8A8_TYPELESS : ColorFormat(f); }
DXGI_FORMAT ColorReadFormat(uint32_t f) { return f <= 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : ColorFormat(f); }
bool WideColor(uint32_t f) { return f == 5 || f == 7 || f == 12 || f == 15; }

// D3D12 compare functions are the console's plus one
D3D12_COMPARISON_FUNC Compare(uint32_t f) { return D3D12_COMPARISON_FUNC(f + 1); }

// how a draw past the volumetric fog's pass takes it, by its colour's blend factors
uint8_t FogMode(uint32_t blend_control) {
    constexpr uint32_t kOne = 1;
    constexpr uint32_t kSrcColour = 4;
    constexpr uint32_t kInvSrcAlpha = 7;
    constexpr uint32_t kDestColour = 8;
    uint32_t src = Bits(blend_control, 0, 5);
    uint32_t dst = Bits(blend_control, 8, 5);
    if (dst == kOne) return fcip::kFogFade;
    if (src == kDestColour || dst == kSrcColour) return fcip::kFogWhite;
    if (src == kOne && dst == kInvSrcAlpha) return fcip::kFogScatterAlpha;
    return fcip::kFogScatter;
}

D3D12_BLEND BlendFactor(uint32_t f, bool alpha) {
    switch (f) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 6: return D3D12_BLEND_SRC_ALPHA;
        case 7: return D3D12_BLEND_INV_SRC_ALPHA;
        case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 10: return D3D12_BLEND_DEST_ALPHA;
        case 11: return D3D12_BLEND_INV_DEST_ALPHA;
        case 12:
        case 14: return D3D12_BLEND_BLEND_FACTOR;
        case 13:
        case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP BlendOp(uint32_t f) {
    switch (f) {
        case 1: return D3D12_BLEND_OP_SUBTRACT;
        case 2: return D3D12_BLEND_OP_MIN;
        case 3: return D3D12_BLEND_OP_MAX;
        case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp) {
    switch (clamp) {
        case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3:
        case 5:
        case 7: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

// what a texture fetch constant points at, uploaded once per change of its guest bytes
struct Texture {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t hash = 0;
    uint64_t checked = ~0ull;  // present it was last hashed in
    uint8_t flags = 0;         // Extension::Decoded's
    // the guest bytes it covers, hashed ahead for the present in hashed_for
    xenos::TextureFetch guest;
    std::vector<xenos::Level> guest_levels;
    uint64_t next_hash = 0;
    uint64_t hashed_for = ~0ull;
    std::atomic<uint32_t> next_state = 0;  // HashState of next_hash
    // the key the texture pack and the dumps know the bytes by, the texturedumper::Generation it was taken at, and
    // the pack image drawn in place of the guest's
    uint64_t pack_key = 0;
    uint32_t pack_gen = 0;
    Texture* swap = nullptr;
};

// who hashes a texture ahead: a pool thread or, when it is wanted first, the render thread
enum HashState : uint32_t { kHashWaiting, kHashTaken, kHashDone };
void TakeHash(Texture& tex);

// a resolve's copy, read in place of the guest texture at its address
struct Resolved {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;  // title texels
    uint32_t height = 0;
    uint32_t view = 0;  // RTV slot
    float scale = 1;    // host texels per title texel
};

constexpr int kReadBacks = 4;  // a frame's resolves to one address, and the one a frame back

// a resolve the CPU reads: a copy at title size each time the title resolves there
struct ReadBack {
    ComPtr<ID3D12Resource> buffer[kReadBacks];
    uint64_t seq[kReadBacks] = {};    // which resolve to the address each holds
    uint64_t fence[kReadBacks] = {};  // the submission it lands in
    uint32_t next = 0;
    uint64_t frame_start = 0;   // resolves issued to the address when the title's frame began
    uint64_t last_frame = 0;    // those the frame before issued
    uint64_t frame_before = 0;  // and the one before that
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;  // the resolve's title texels
    uint32_t height = 0;
    uint32_t copy_width = 0;  // with the blocks it wrote past them
    uint32_t copy_height = 0;
    uint32_t row_pitch = 0;
};

// the title's CPU reads of resolves, which the render thread answers in command order
struct CpuReads {
    std::mutex lock;
    std::condition_variable ran;
    std::unordered_map<uint32_t, uint64_t> issued;  // resolves the title issued, by address
    std::unordered_map<uint32_t, uint64_t> done;    // those the render thread ran
    std::unordered_map<uint32_t, ReadBack> read;    // addresses the CPU reads
    ID3D12Fence* fence = nullptr;
    uint64_t queued = 0;  // Copy calls
    uint64_t run = 0;     // those the render thread ran
};
CpuReads g_cpu;

constexpr int32_t kResolveBlock = 8;  // the library rounds a resolve's right and bottom edges up to this

// everything a pipeline depends on beyond the root signature
struct PipelineKey {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t rt_formats[4] = {};
    uint32_t ds_format = 0;
    uint32_t samples = 1;
    uint32_t blend[4] = {};
    uint32_t color_mask = 0;
    uint32_t color_control = 0;
    uint32_t depth_control = 0;
    uint32_t stencil = 0;  // read and write masks
    uint32_t su_mode = 0;
    float poly_scale = 0;
    uint32_t topology = 0;
    uint32_t strip_cut = 0;
    uint32_t stream_out = 0;  // kStreamRtao or a kStreamCaster
    uint32_t motion = 0;      // kMotionTarget's format, 0 none
    uint32_t albedo = 0;      // fcip::kAlbedoTarget's use, kAlbedo*
    uint32_t depth_only = 0;  // no colour written; the key is hashed and compared as bytes, so no padding
};
enum : uint32_t { kAlbedoWrite = 1, kAlbedoClear = 2 };

// cache/pipelines.bin: the header, then per pipeline its record, the shaders' DXIL and the driver's blob
constexpr uint32_t kPipelineCacheMagic = 0x33505358;
struct PipelineRecord {
    PipelineKey key;
    uint32_t vs_size = 0;
    uint32_t ps_size = 0;
    uint32_t blob_size = 0;
    uint32_t unused = 0;  // no padding: written as bytes
};

// a pipeline read from the cache, pointing into the file's bytes
struct CachedPipeline {
    PipelineKey key;
    D3D12_SHADER_BYTECODE vs{};
    D3D12_SHADER_BYTECODE ps{};
    D3D12_CACHED_PIPELINE_STATE blob{};
};

// a pipeline for a build thread to make, handed back with what it made
struct BuiltPipeline {
    PipelineKey key;
    D3D12_SHADER_BYTECODE vs{};
    D3D12_SHADER_BYTECODE ps{};
    ComPtr<ID3D12PipelineState> pso;
};


struct Frame {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12Resource> upload;
    uint8_t* map = nullptr;
    uint64_t used = 0;
    uint32_t views = 0;
    uint64_t fence = 0;
    std::vector<ComPtr<ID3D12Resource>> garbage;
};

// a draw's render targets as OMSetRenderTargets takes them, 0 for none
struct TargetViews {
    SIZE_T rtv[fcip::kAlbedoTarget + 1] = {};
    SIZE_T dsv = 0;
};

struct Alloc {
    ID3D12Resource* res;
    uint64_t offset;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
};

// Fenced draws read guest memory the title leaves alone until a fence past them passes, so what a fenced draw
// uploaded is used again by later ones until a fence passes or the upload ring starts over (an epoch)
constexpr size_t kKeptUploads = 4096;  // a power of two

struct KeptVertices {
    const uint8_t* data = nullptr;
    uint64_t first = 0;
    uint64_t end = 0;
    uint64_t epoch = 0;
    uint64_t offset = 0;  // in the upload ring
};

struct KeptIndices {
    const uint8_t* data = nullptr;
    uint32_t count = 0;
    uint32_t format = 0;  // index32, then the endian from bit 1
    uint64_t epoch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t min = 0;
    uint32_t max = 0;
};

size_t KeptSlot(const void* data, uint64_t a, uint64_t b) {
    uint64_t h = (uint64_t(reinterpret_cast<uintptr_t>(data)) + a * 31 + b) * 0x9E3779B97F4A7C15ull;
    return size_t(h >> 52) & (kKeptUploads - 1);
}

struct Query {
    uint64_t fence = 0;  // submission the result is resolved in, 0 while recording
    bool open = false;
};

const char* kResolveHlsl = R"(
cbuffer R : register(b0) { float4 src_rect; float4 opts; float4 colour[3]; };
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2(id & 1, id >> 1);
    o.p = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    o.uv = src_rect.xy + t * src_rect.zw;
    return o;
}
#if MS
Texture2DMS<float4> img : register(t0);
#else
Texture2D<float4> img : register(t0);
SamplerState ls : register(s0);
#endif
float4 ps(O i) : SV_Target {
    int2 at = int2(i.uv);
#if MS
    float4 c = 0;
    for (uint s = 0; s < uint(opts.x); s++) c += img.Load(at, s);
    c /= opts.x;
#elif FILTER
    float2 size;
    img.GetDimensions(size.x, size.y);
    float4 c = img.SampleLevel(ls, i.uv / size, 0);
#else
    float4 c = img.Load(int3(at, 0));
#endif
    c = opts.y != 0 ? c.xxxx : c;
    c.rgb *= opts.z != 0 ? opts.z : 1;
    // opts.w: the rows hold a colour matrix, applied to linear colour
    if (opts.w > 0) {
        float3x3 m = float3x3(colour[0].xyz, colour[1].xyz, colour[2].xyz);
        c.rgb = pow(saturate(mul(m, pow(saturate(c.rgb), 2.2))), 1 / 2.2);
    }
    return c;
}
)";

// Context::Blit: taps per axis averaged, colour times mul, sRGB encoded when asked
// HDR10 output: the frame with the panel over it, its highlights stretched towards the display's peak
// AMD CAS around ffx_a.h and ffx_cas.h: the upscaled frame sharpened in place, a pixel each
const char* kCasDefines = "#define A_GPU 1\n#define A_HLSL 1\n";
const char* kCasLoad = R"(
cbuffer P : register(b1) { uint4 const1; float4 alpha_shift; };
Texture2D img : register(t0);
Texture2D scene : register(t1);
SamplerState ls : register(s0);
AF3 CasLoad(ASU2 p) { return img.Load(int3(p, 0)).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";
const char* kCasPass = R"(
struct O { float4 p : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 cas(O i) : SV_Target {
    AF3 c;
    // sharpening only reads no scaling constants
    CasFilter(c.r, c.g, c.b, AU2(i.p.xy), AU4(0, 0, 0, 0), const1, true);
    // FSR keeps no alpha, which holds the game's glow mask: the scene's own, unjittered
    float2 size;
    img.GetDimensions(size.x, size.y);
    return float4(c, scene.SampleLevel(ls, i.p.xy / size + alpha_shift.xy, 0).a);
}
)";

const char* kHdrHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D frame : register(t0);
Texture2D ui : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
// gamma 2.2 as SDR monitors show it; sRGB's linear toe lifts the darks
float3 to_linear(float3 c) {
    return pow(c, 2.2);
}
// inverse Reinhard on luminance so hues keep: the brighter, the more it gains, white reaches most
float3 expand(float3 c, float most) {
    return c / (1 - dot(c, kLum) * (1 - 1 / most));
}
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
// p0: SDR white nits, peak nits
float4 hdr10(O i) : SV_Target {
    int3 px = int3(i.p.xy, 0);
    float3 f = min(expand(to_linear(frame.Load(px).rgb), max(p0.y / p0.x, 1)) * p0.x, p0.y);
    float4 u = ui.Load(px);
    float3 panel = to_linear(u.a > 0 ? u.rgb / u.a : 0) * p0.x;
    return float4(pq(mul(k709To2020, lerp(f, panel, u.a))), 1);
}
)";

const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

// the scene's view space from its depth, shared by ambient occlusion and global illumination
const char* kViewHlsl = R"(
cbuffer A : register(b1) {
    float4 proj;
    float4 zp;
    float4 ndc;
    float4 vp;
    float4 opt;
    float4 fog;
    float4 fogcolor;
    float4 amb;
    float4 sun[4];
    float4 height;
    float4 suncolour;
    float4 terrain[2];
};
#ifdef MS
Texture2DMS<float> depth : register(t0);
float Raw(int2 p) { return depth.Load(p, 0); }
#else
Texture2D<float> depth : register(t0);
float Raw(int2 p) { return depth.Load(int3(p, 0)); }
#endif
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
// view space position of a pixel, z the view distance. proj: 1 / scale xy, centre xy; zp: z = x / (raw - y) of the
// host's depth; ndc: a pixel centre to the title's ndc; vp: the scissor in pixels
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 n = (p + 0.5) * ndc.xy + ndc.zw;
    return float3((n - proj.zw) * proj.xy * z, z);
}
// pixels a world radius spans at view distance z
float2 Span(float z) { return opt.x / (proj.xy * z * abs(ndc.xy)); }
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 pos, float3 c) {
    float3 l = View(pos - float2(1, 0));
    float3 r = View(pos + float2(1, 0));
    float3 u = View(pos - float2(0, 1));
    float3 d = View(pos + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// 1 where the sun reaches p, a view space point, through the far sun map; off the map counts as lit
float SunLit(Texture2D<float> map, float3 p) {
    float4 at = float4(p, 1);
    float4 c = float4(dot(at, sun[0]), dot(at, sun[1]), dot(at, sun[2]), dot(at, sun[3]));
    float3 n = c.xyz / c.w;
    float2 uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1) || n.z <= 0 || n.z >= 1) return 1;
    uint w;
    uint h;
    map.GetDimensions(w, h);
    // reversed depth: a caster nearer the sun is deeper
    return n.z >= map.Load(int3(int2(uv * float2(w, h)), 0)) - 0.001 ? 1 : 0;
}
)";

// Ambient occlusion from the scene's depth: occlusion pass, then a blur across and one down that multiplies it into
// the colour. The sky is far: a pixel's samples there span less than one pixel and it stays lit
const char* kAoHlsl = R"(
Texture2D<float2> ao : register(t1);
// opt: world radius, most pixels it may span; out: occlusion, view distance
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = Span(c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16);
    float a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16;
        float t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v);
        float w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = Span(c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
#ifdef RT
// RTAO: 4 cosine weighted rays a pixel, turned and spread by the 4x4 dither, each ending at the nearer of the world's
// triangles and the screen's depth, which alone holds alpha tested foliage; falloff and power as gtao's. opt.x the
// ray length
RaytracingAccelerationStructure scene : register(t2);
// where the ray first passes behind the screen's depth by less than its length, in 4 steps short of end
float ScreenHit(float3 o, float3 d, float end, float jitter) {
    for (int j = 0; j < 4; j++) {
        float t = (j + jitter) / 4 * opt.x;
        float3 p = o + d * t;
        if (t >= end || p.z <= 0) break;
        float2 px = (p.xy / (proj.xy * p.z) + proj.zw - ndc.zw) / ndc.xy;
        if (any(px < vp.xy) || any(px >= vp.xy + vp.zw)) break;
        float behind = p.z - View(px).z;
        if (behind > 0.01 + p.z * 0.002 && behind < opt.x) return t;
    }
    return end;
}
float2 rtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = Span(c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    if (dot(n, c) > 0) n = -n;
    float3 t = normalize(cross(n, abs(n.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 b = cross(n, t);
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16);
    float spread = kDither[cell.x * 4 + cell.y] / 16;
    RayDesc ray;
    // a pixel's width off the surface, past the depth's own error
    ray.Origin = c + n * (opt.x / span + 0.01);
    ray.TMin = 0;
    ray.TMax = opt.x;
    float occluded = 0;
    for (int i = 0; i < 4; i++) {
        float u = (i + spread) / 4;
        float phi = i * 2.3999632 + turn;
        float r = sqrt(u);
        ray.Direction = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1 - u);
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        float hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : opt.x;
        hit = ScreenHit(ray.Origin, ray.Direction, hit, frac(spread + i * 0.618));
        occluded += saturate((opt.x - hit) / (opt.x * 0.615));
    }
    return float2(pow(1 - occluded / 4, 2.2), c.z);
}
#endif
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float Smooth(float2 pos, out float z) {
    int2 p = int2(pos);
    int2 lo = int2(vp.xy);
    int2 hi = int2(vp.xy + vp.zw) - 1;
    z = ao.Load(int3(p, 0)).y;
    float sum = 0;
    float n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float2 occlusion_blur(float4 pos : SV_Position) : SV_Target {
    float z;
    float a = Smooth(pos.xy, z);
    return float2(a, z);
}
// the fog is already drawn: only the scene share under it is occluded, blended as colour * a + rgb
float4 occlusion_apply(float4 pos : SV_Position) : SV_Target {
    float z;
    float a = Smooth(pos.xy, z);
    float fogged = saturate(fog.x * z + fog.y);
    return float4(fogcolor.rgb * fogged * (1 - a), a);
}
)";

// Global illumination: one bounce of the light the scene's surfaces throw onto each other. 4 cosine weighted rays a
// pixel at half resolution through the camera pass's world, turned and spread by the 4x4 dither. A hit the screen
// shows takes the colour seen there; a hidden or off screen one is lit as the game lights it, its texture's colour
// under the ambient and the sun through the far sun map. The game lights everything with a flat ambient, its sky: the
// share of rays that hit something takes that share of it away, in the game's own colour space; in the sun's shadow,
// where all the colour is indirect, that share of the whole colour. Blurred across at half
// resolution and down at full within depth edges, then the pixel's albedo times the bounce is added under the fog.
// Light adds in linear space: the scene's colour and the textures are gamma encoded. opt.x the ray length, opt.y the
// scene's gamma, 1 where its target holds linear colour, amb the game's ambient, w 1 with the sun map bound. Then come
// fcip::kCasterHitHlsl and kGiHlsl
const char* kGiViewsHlsl = R"(
Texture2D<float4> light : register(t1);
RaytracingAccelerationStructure scene : register(t2);
#ifdef MS
Texture2DMS<float4> albedo : register(t3);
float4 Albedo(int2 p) { return albedo.Load(p, 0); }
#else
Texture2D<float4> albedo : register(t3);
float4 Albedo(int2 p) { return albedo.Load(int3(p, 0)); }
#endif
Texture2D<float4> colour : register(t4);  // the scene before it, resolved
float3 Linear(float3 c) { return pow(max(c, 0), opt.y); }
ByteAddressBuffer t_ray_casters : register(t5);
ByteAddressBuffer t_ray_records : register(t6);
Texture2D<float> sunmap : register(t7);
Texture2D<float4> medium : register(t8);  // the volumetric fog's when it went in before, else none
// the share of the pixel the fog in front of it leaves
float FogLeft(float2 pos, float z) { return exp(-medium.Load(int3(pos, 0)).a * z); }
)";
const char* kGiHlsl = R"(
static const float kHitLod = 4;    // mips down a hit's texture is read: its average colour, cheap
static const float kOcclusion = 0.6;  // share of the blocked indirect light taken away
// the full resolution pixel a half resolution one traces for
int2 Full(int2 h) { return h * 2; }
// the screen's colour at p, a view space point, where the screen sees that point
bool Seen(float3 p, out float3 seen) {
    seen = 0;
    if (p.z <= 0) return false;
    float2 px = (p.xy / (proj.xy * p.z) + proj.zw - ndc.zw) / ndc.xy;
    if (any(px < vp.xy) || any(px >= vp.xy + vp.zw)) return false;
    if (abs(View(px).z - p.z) > 0.05 + p.z * 0.02) return false;
    seen = Linear(colour.Load(int3(px, 0)).rgb);
    return true;
}
float3 Unpack(uint rgb) { return float3(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255) / 255; }
// the light leaving a hit at p along dir as the game lights it: its texture's colour, grey without one, under its
// material's colour, its draw's ambient, else the camera's, and, where its draw takes it, the sun
float3 Bounced(uint caster, uint prim, float2 bary, float3 p, float3 dir) {
    uint4 c = t_ray_casters.Load4(caster * 32);
    uint3 lit = t_ray_casters.Load3(caster * 32 + 16);
    uint at = (c.x + prim * 3) * 32;
    float3 a = asfloat(t_ray_records.Load3(at));
    float3 e = cross(asfloat(t_ray_records.Load3(at + 32)) - a, asfloat(t_ray_records.Load3(at + 64)) - a);
    float3 n = e * rsqrt(max(dot(e, e), 1e-20));
    if (dot(n, dir) > 0) n = -n;
    float3 albedo = 0.5;
    if (c.w & 8) {
        float4 ta = asfloat(t_ray_records.Load4(at + 16));
        float4 tb = asfloat(t_ray_records.Load4(at + 48));
        float4 td = asfloat(t_ray_records.Load4(at + 80));
        float4 tex = ta + (tb - ta) * bary.x + (td - ta) * bary.y;
        float2 uv = float2(tex[c.w >> 4 & 3], tex[c.w >> 6 & 3]);
        albedo = t_heap[NonUniformResourceIndex(c.y)].SampleLevel(s_heap, uv, kHitLod).rgb;
    }
    albedo *= Unpack(lit.x);
    float3 incoming = (lit.z >> 24) != 0 ? Unpack(lit.z) : amb.rgb;
    if (amb.w > 0 && lit.y != 0) {
        float facing = saturate(dot(n, normalize(sun[2].xyz)));
        incoming += suncolour.rgb * facing * SunLit(sunmap, p + n * 0.05);
    }
    // no brighter than the screen could show it: the sun's colour lights past 1 in the game's space
    return Linear(saturate(albedo * incoming));
}
// out: the light reaching the pixel, the share of its rays that reach the sky; -1 where no draw left an albedo
float4 gi_trace(float4 pos : SV_Position) : SV_Target {
    int2 full = Full(int2(pos.xy));
    if (Albedo(full).a == 0) return float4(0, 0, 0, -1);
    float3 c = View(full);
    float pixel = c.z * proj.x * abs(ndc.x);
    // a pixel this wide sees nothing within the rays' length, the sky
    if (pixel > opt.x * 0.25) return float4(0, 0, 0, 1);
    float3 n = Normal(full, c);
    if (dot(n, c) > 0) n = -n;
    float3 t = normalize(cross(n, abs(n.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 b = cross(n, t);
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16);
    float spread = kDither[cell.x * 4 + cell.y] / 16;
    RayDesc ray;
    // two pixels off the surface, past the depth's own error
    ray.Origin = c + n * (pixel * 2 + 0.01);
    ray.TMin = 0;
    ray.TMax = opt.x;
    float3 sum = 0;
    float sky = 0;
    for (int i = 0; i < 4; i++) {
        float u = (i + spread) / 4;
        float phi = i * 2.3999632 + turn;
        float r = sqrt(u);
        ray.Direction = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1 - u);
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed()) {
            uint caster = q.CandidateInstanceID() + q.CandidateGeometryIndex();
            float2 bary = q.CandidateTriangleBarycentrics();
            if (CasterHit(caster, q.CandidatePrimitiveIndex(), bary)) q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            sky += 1;
            continue;
        }
        float3 p = ray.Origin + ray.Direction * q.CommittedRayT();
        float3 seen;
        if (Seen(p, seen)) {
            sum += seen;
            continue;
        }
        uint caster = q.CommittedInstanceID() + q.CommittedGeometryIndex();
        sum += Bounced(caster, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), p, ray.Direction);
    }
    return float4(sum / 4, sky / 4);
}
// h a half resolution pixel weighed against view distance z; opt.zw: step; 9 taps, twice the dither's period, depth
// edges and pixels without albedo left out
float4 Smooth(int2 h, float z) {
    int2 lo = int2(vp.xy) / 2;
    int2 hi = (int2(vp.xy + vp.zw) + 1) / 2 - 1;
    float4 sum = 0;
    float n = 0;
    for (int i = -4; i <= 4; i++) {
        int2 at = clamp(h + i * int2(opt.zw), lo, hi);
        float4 s = light.Load(int3(at, 0));
        if (s.w < 0) continue;
        float w = (abs(i) == 4 ? 0.5 : 1) * saturate(2 - abs(View(Full(at)).z - z) / (z * 0.02));
        sum += s * w;
        n += w;
    }
    return n > 0 ? sum / n : float4(0, 0, 0, 1);
}
float4 gi_blur(float4 pos : SV_Position) : SV_Target {
    int2 h = int2(pos.xy);
    if (light.Load(int3(h, 0)).w < 0) return float4(0, 0, 0, -1);
    return Smooth(h, View(Full(h)).z);
}
// how much the sun lights the pixel, read from the game's own shading: its colour over its albedo, the light it took,
// against the ambient and the sun; 1 without the sun map
float SunReach(float2 pos, float3 c, float3 albedo) {
    if (amb.w == 0) return 1;
    float3 p = View(pos);
    float3 n = Normal(pos, p);
    if (dot(n, p) > 0) n = -n;
    float facing = max(dot(n, normalize(sun[2].xyz)), 0.25);
    float3 luma = float3(0.299, 0.587, 0.114);
    float light = dot(c, luma) / max(dot(albedo, luma), 0.02);
    return saturate((light - dot(amb.rgb, luma)) / (dot(suncolour.rgb, luma) * facing));
}
// the scene's colour lit again, written over it with its alpha kept; the half resolution light is taken up within
// the pixel's depth
float4 gi_apply(float4 pos : SV_Position) : SV_Target {
    float z = View(pos.xy).z;
    float4 l = Smooth(int2(pos.xy) / 2, z);
    float4 a = Albedo(int2(pos.xy));
    float keep = a.a * (1 - saturate(fog.x * z + fog.y)) * FogLeft(pos.xy, z);
    float3 c = colour.Load(int3(pos.xy, 0)).rgb;
    // the pixel's indirect light: all of it in the sun's shadow, the game's ambient where the sun reaches
    float3 indirect = lerp(c, min(c, a.rgb * amb.rgb), SunReach(pos.xy, c, a.rgb));
    float3 lit = c - indirect * (1 - l.w) * kOcclusion * keep;
    float3 bounce = pow(a.rgb, 2.2) * l.rgb * keep;
    return float4(pow(Linear(lit) + bounce, 1 / opt.y), 0);
}
)";
// Volumetric fog in place of the game's: humid height fog marched through the sun map and the terrain shade, thickest
// at the sea, over the sky too; lit by the fog colour around and the sun ahead, scattered forward as wet air does.
// colour * transmittance + in scatter. opt.x: 1 with the sun map bound, opt.y with the terrain shade
const char* kFogHlsl = R"(
Texture2D<float4> haze : register(t1);  // in scatter, transmittance in a
Texture2D<float> sunmap : register(t2);
Texture2D<float> shade : register(t3);  // height over the sea below which the terrain hides the sun
static const int kSteps = 16;
static const float kSky = 250;          // world units of fog in front of the sky
static const float kDensity = 3;        // over the game's fog slope: about 95% fog where the game's ends
static const float kThinning = 0.015;   // density falloff a unit up
static const float3 kHumid = float3(0.92, 1, 1.06);  // wet air's faint aqua over the fog colour
static const float kGround = 0.25;      // units a lit ground point may sit under its shade texel
// Henyey Greenstein, 1 averaged over the sphere
float Phase(float mu, float g) { return (1 - g * g) / pow(1 + g * g - 2 * g * mu, 1.5); }
// no map counts as shaded
float FogSunLit(float3 p) { return opt.x == 0 ? 0 : SunLit(sunmap, p); }
// 1 where no terrain stands between p, up over the sea, and the sun; off the shade counts as lit
float TerrainLit(float3 p, float up) {
    if (opt.y == 0) return 1;
    float4 at = float4(p, 1);
    int2 t = int2(round(float2(dot(at, terrain[0]), dot(at, terrain[1]))));
    uint w;
    uint h;
    shade.GetDimensions(w, h);
    if (any(t < 0) || any(t >= int2(w, h))) return 1;
    return up + kGround >= shade.Load(int3(t, 0)) ? 1 : 0;
}
// a pixel's view space point; the sky, depth 0, kSky away
float3 Seen(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    if (Raw(p) > 0) return View(px);
    float2 n = (p + 0.5) * ndc.xy + ndc.zw;
    float3 dir = float3((n - proj.zw) * proj.xy, 1);
    return dir * (kSky / length(dir));
}
float4 fog_march(float4 pos : SV_Position) : SV_Target {
    float3 c = Seen(pos.xy);
    float dist = length(c);
    // the sun map's depth grows toward the sun
    float3 axis = sun[2].xyz;
    float mu = dot(axis, axis) > 0 ? dot(c / dist, normalize(axis)) : 0;
    // a bright lobe toward the sun over a faint wide one
    float3 sunlight = suncolour.rgb * (0.12 * Phase(mu, 0.75) + 0.05 * Phase(mu, 0.2));
    float3 ambient = fogcolor.rgb * kHumid;
    float sigma = max(fog.x * kDensity, 2e-4);
    int2 cell = int2(pos.xy) & 3;
    float jitter = kDither[cell.y * 4 + cell.x] / 16;
    float3 light = 0;
    float trans = 1;
    for (int i = 0; i < kSteps; i++) {
        float s = (i + jitter) / kSteps;
        float3 p = c * (s * s);
        float up = dot(float4(p, 1), height);
        // steps spread by s squared: this one spans (2i + 1) / kSteps^2 of the distance
        float ext = sigma * exp(-kThinning * max(up, 0)) * dist * (2 * i + 1) / (kSteps * kSteps);
        light += trans * ext * (sunlight * FogSunLit(p) * TerrainLit(p, up) + ambient);
        trans *= exp(-ext);
    }
    return float4(light, trans);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float4 Smooth(float2 pos) {
    int2 p = int2(pos);
    int2 lo = int2(vp.xy);
    int2 hi = int2(vp.xy + vp.zw) - 1;
    float z = Seen(pos).z;
    float4 sum = 0;
    float n = 0;
    for (int i = -2; i <= 2; i++) {
        int2 at = clamp(p + i * int2(opt.zw), lo, hi);
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(Seen(at).z - z) / (z * 0.02));
        sum += haze.Load(int3(at, 0)) * w;
        n += w;
    }
    return sum / n;
}
float4 fog_blur(float4 pos : SV_Position) : SV_Target { return Smooth(pos.xy); }
float4 fog_apply(float4 pos : SV_Position) : SV_Target { return Smooth(pos.xy); }
// the medium the draws past the fog look through, as if even along the pixel's ray: the fog's colour over what it
// hides, and its extinction a unit of view distance
float4 fog_medium(float4 pos : SV_Position) : SV_Target {
    float4 h = Smooth(pos.xy);
    float hidden = 1 - h.a;
    float3 colour = hidden > 1e-4 ? h.rgb / hidden : 0;
    return float4(colour, -log(max(h.a, 1e-6)) / Seen(pos.xy).z);
}
)";

// Tropical cumulus over the sky box, marched at half size along each pixel's view ray through a layer curving with the
// earth, then blended into the pixels nothing but sky covers. They sit in the sky box's frame, turned by height.xy,
// and drift on the wind. sun[0..2] take a view direction into the world, sun[3] points at the sun, height.z is the
// camera's height over the sea and height.w the wind's seconds, terrain[0] coverage and rain, opt.xy the march's size
const char* kCloudHlsl = R"(
Texture2D<float4> march : register(t1);  // in scatter, transmittance in a
static const float kBase = 700;           // the layer over the sea, world units
static const float kTop = 2000;
static const float kEarth = 600000;       // the layer's curve, thinning it toward the horizon
static const float kFar = 30000;          // the farthest a ray enters the layer
static const float kSpan = 8000;          // the most of the layer one ray crosses
static const float kCell = 1100;          // a cloud's size
static const float kLow = 0.15;           // rise a unit along the ray where clouds reach full strength
static const float kBillow = 260;         // the billows eroding its edges
static const float kSigma = 0.02;         // extinction a unit at full density
static const float kFade = 14000;         // distance over which clouds sink into the sky's haze
static const float kSunGain = 0.6;
static const float2 kWind = float2(7, 3);  // units a second
static const float kGrowth = 16;           // units a second the shapes form and fade by
static const float kChurn = 36;            // the same for the billows on their edges
static const int kSteps = 40;
static const int kLightSteps = 4;
float Hash(float3 p) {
    p = frac(p * 0.3183099 + 0.1) * 17;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
// value noise, smooth between lattice points
float Noise(float3 p) {
    float3 i = floor(p);
    float3 f = p - i;
    f = f * f * (3 - 2 * f);
    float x00 = lerp(Hash(i), Hash(i + float3(1, 0, 0)), f.x);
    float x10 = lerp(Hash(i + float3(0, 1, 0)), Hash(i + float3(1, 1, 0)), f.x);
    float x01 = lerp(Hash(i + float3(0, 0, 1)), Hash(i + float3(1, 0, 1)), f.x);
    float x11 = lerp(Hash(i + float3(0, 1, 1)), Hash(i + float3(1, 1, 1)), f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}
// Henyey Greenstein, 1 averaged over the sphere
float Phase(float mu, float g) { return (1 - g * g) / pow(1 + g * g - 2 * g * mu, 1.5); }
float3 ToWorld(float3 d) { return d.x * sun[0].xyz + d.y * sun[1].xyz + d.z * sun[2].xyz; }
// a world direction in the sky box's frame, which the box turned about z
float3 ToSky(float3 d) { return float3(d.x * height.x + d.y * height.y, d.y * height.x - d.x * height.y, d.z); }
// distance along a ray rising up a unit to the shell h over the sea, from below it
float Shell(float h, float up) {
    float centre = kEarth + height.z;
    float b = centre * up;
    float c = (height.z - h) * (2 * kEarth + height.z + h);
    return max(sqrt(max(b * b - c, 0)) - b, 0);
}
// height over the sea t along a ray rising up a unit
float Altitude(float t, float up) {
    float along = kEarth + height.z + t * up;
    return sqrt(along * along + t * t * (1 - up * up)) - kEarth;
}
// 0 at the layer's base, 1 at its top
float Rise(float h) { return (h - kBase) / (kTop - kBase); }
// density at p in the sky's frame, p.z over the sea: flat bases, towers narrowing as they rise, billows eating their
// edges; the sunlight's march reads the shape alone
float Density(float3 p, bool billows) {
    float r = Rise(p.z);
    if (r <= 0 || r >= 1) return 0;
    float3 q = float3(p.xy + kWind * height.w, p.z + kGrowth * height.w);
    // the finer octave grows against the coarse one, so shapes change rather than scroll
    float3 against = float3(q.xy, q.z - 2.5 * kGrowth * height.w);
    float shape = Noise(q / kCell) * 0.65 + Noise(against / (kCell * 0.37) + 7.1) * 0.35;
    float d = saturate((shape - (1 - terrain[0].x) - 0.3 * r * r) * 6) * saturate(r * 10);
    if (!billows || d <= 0) return d;
    q.z += kChurn * height.w;
    float billow = Noise(q / kBillow) * 0.6 + Noise(q / (kBillow * 0.4)) * 0.4;
    return saturate(d - billow * 0.45 * (1 - d));
}
// sunlight left at p past the cloud toward the sun, some of it scattered on through
float SunThrough(float3 p, float3 toward) {
    float depth = 0;
    float stride = 120;
    for (int i = 0; i < kLightSteps; i++) {
        p += toward * stride;
        depth += Density(p, false) * stride;
        stride *= 2;
    }
    float tau = depth * kSigma;
    return max(exp(-tau), 0.3 * exp(-0.25 * tau));
}
float4 cloud_march(float4 pos : SV_Position) : SV_Target {
    float2 n = pos.xy * 2 * ndc.xy + ndc.zw;
    float3 dir = ToSky(normalize(ToWorld(float3((n - proj.zw) * proj.xy, 1))));
    if (dir.z <= 0.005) return float4(0, 0, 0, 1);
    float t0 = Shell(kBase, dir.z);
    if (t0 >= kFar) return float4(0, 0, 0, 1);
    float t1 = min(Shell(kTop, dir.z), t0 + kSpan);
    float3 toward = ToSky(sun[3].xyz);
    float mu = dot(dir, toward);
    float rain = terrain[0].y;
    float3 sunlight = suncolour.rgb * kSunGain * (0.8 * Phase(mu, 0.55) + 0.2 * Phase(mu, -0.2)) * (1 - 0.65 * rain);
    // the sky's haze around them, greyer and dimmer in rain
    float grey = dot(fogcolor.rgb, float3(0.3, 0.59, 0.11));
    float3 around = lerp(fogcolor.rgb, grey.xxx, 0.35 + 0.4 * rain) * (1 - 0.35 * rain);
    float dt = (t1 - t0) / kSteps;
    float jitter = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
    float3 light = 0;
    float trans = 1;
    for (int i = 0; i < kSteps && trans > 0.02; i++) {
        float t = t0 + (i + jitter) * dt;
        float3 p = float3(dir.xy * t, Altitude(t, dir.z));
        float d = Density(p, true);
        if (d <= 0) continue;
        float absorbed = 1 - exp(-d * kSigma * dt);
        float3 lit = sunlight * SunThrough(p, toward) + around * (0.6 + 0.6 * saturate(Rise(p.z)));
        light += trans * absorbed * lit;
        trans *= 1 - absorbed;
    }
    // the sky overhead carries them, the horizon only thinly
    float fade = exp(-t0 / kFade) * smoothstep(0, kLow, dir.z);
    return float4(light * fade, lerp(1, trans, fade));
}
// the march's texel at p, held to its size
float4 Marched(int2 p) { return march.Load(int3(clamp(p, 0, int2(opt.xy) - 1), 0)); }
float4 cloud_apply(float4 pos : SV_Position) : SV_Target {
    if (Raw(int2(pos.xy)) > 0) return float4(0, 0, 0, 1);
    float2 at = pos.xy * 0.5 - 0.5;
    int2 i = int2(floor(at));
    float2 f = at - i;
    float4 top = lerp(Marched(i), Marched(i + int2(1, 0)), f.x);
    float4 bottom = lerp(Marched(i + int2(0, 1)), Marched(i + int2(1, 1)), f.x);
    return lerp(top, bottom, f.y);
}
)";

// Caustics: the sunlight a tropical sea's waves gather into thin bright ribbons on whatever lies under them. Two sheets
// of drifting cells glow along the edges between them, bent into curves, the finer one turned, quicker and fainter, in
// slow patches of brighter and duller light, laid on the sea's plane along the sun; deeper water spreads them wider.
// opt.x: 1 with the sun map bound; fog.xyz toward the sun in the world, fog.w the seconds; terrain[0], terrain[1] and
// height take view space to the world's x, y and height over the sea
const char* kCausticsHlsl = R"(
Texture2D<float> sunmap : register(t2);
static const float kCell = 1.2;        // world units between the coarse sheet's ribbons
static const float kFine = 0.55;       // the finer sheet's cells, of the coarse one's
static const float kRibbon = 0.07;     // a ribbon's width, of a cell
static const float kDeepBlur = 0.05;   // world units a ribbon widens per unit of water over it
static const float kPatch = 0.09;      // the brighter and duller patches, per world unit
static const float kStrength = 0.5;    // light a ribbon adds, of the sun's
static const float3 kSea = float3(0.9, 1, 0.97);  // the sun's colour through clear shallow water
// a cell's point, wandering about its centre
float2 CellPoint(int2 cell, float seconds) {
    uint h = uint(cell.x) * 0x8DA6B343u ^ uint(cell.y) * 0xD8163841u;
    h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
    h ^= h >> 13;
    float2 phase = float2(h & 0xFFFF, h >> 16) * (6.2831853 / 65536);
    return cell + 0.5 + 0.35 * sin(seconds * float2(0.9, 0.7) + phase);
}
// cell space bent by two rippling waves, so cell edges curve like light through a moving surface
float2 Warp(float2 q, float seconds) {
    float2 a = sin(q.yx * float2(1.37, 1.13) + seconds * float2(0.41, -0.53));
    float2 b = sin(q.yx * float2(2.71, 2.29) + a * 1.7 + seconds * float2(-0.67, 0.59));
    return q + 0.32 * a + 0.16 * b;
}
// 1 on the edges between the nearest cells' points falling off sharply, blur cells wide or more spread thinner
float Sheet(float2 q, float seconds, float blur) {
    int2 base = int2(floor(q));
    float nearest = 9;
    float second = 9;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float d = length(CellPoint(base + int2(x, y), seconds) - q);
            second = min(second, max(nearest, d));
            nearest = min(nearest, d);
        }
    }
    float width = kRibbon + blur;
    float glow = saturate(1 - (second - nearest) / width);
    return glow * glow * glow * kRibbon / width;
}
// both sheets, brightest where their ribbons cross, dimmed in slow drifting patches
float Ribbons(float2 q, float seconds, float blur) {
    float coarse = Sheet(Warp(q / kCell, seconds), seconds, blur / kCell);
    float2 turned = float2(q.x * 0.8 - q.y * 0.6, q.x * 0.6 + q.y * 0.8) / (kCell * kFine) + 17.3;
    float fine = Sheet(Warp(turned, seconds * 1.3), seconds * 1.3, blur / (kCell * kFine));
    float patch = 0.6 + 0.4 * sin(dot(Warp(q * kPatch, seconds * 0.25), float2(1.9, 1.4)));
    return patch * saturate(0.75 * coarse + 0.25 * fine + 0.5 * coarse * fine);
}
// the light on the pixel at pos, c its view space point, a share of its own colour
float3 CausticsLight(float2 pos, float3 c) {
    float4 at = float4(c, 1);
    float3 world = float3(dot(at, terrain[0]), dot(at, terrain[1]), dot(at, height));
    // the sheets lie on the sea's plane and fall along the sun
    float2 q = world.xy - world.z * fog.xy / max(fog.z, 0.2);
    float blur = length(fwidth(q)) + kDeepBlur * max(-world.z, 0);
    float3 n = Normal(pos, c);
    // a pixel with sky on both sides each way has no normal: taken as facing the camera
    if (!all(isfinite(n))) n = -normalize(c);
    if (dot(n, c) > 0) n = -n;
    float3 up = float3(dot(n, terrain[0].xyz), dot(n, terrain[1].xyz), dot(n, height.xyz));
    float lit = saturate(dot(up, fog.xyz)) * (opt.x == 0 ? 1 : SunLit(sunmap, c));
    return Ribbons(q, fog.w, blur) * suncolour.rgb * kSea * kStrength * lit;
}
// a pixel's view space point; solid false where only sky lies behind it
float3 Floor(float2 pos, out bool solid) {
    solid = Raw(clamp(int2(pos), int2(vp.xy), int2(vp.xy + vp.zw) - 1)) > 0;
    return solid ? View(pos) : float3(0, 0, 1);
}
// the light and the view distance of what it falls on, for the water's draws to take through their screen
float4 caustics_light(float4 pos : SV_Position) : SV_Target {
    bool solid;
    float3 c = Floor(pos.xy, solid);
    float4 light = float4(CausticsLight(pos.xy, c), c.z);
    return solid ? light : (float4)0;
}
// the camera under the sea: straight onto what lies under it
float4 caustics_under(float4 pos : SV_Position) : SV_Target {
    bool solid;
    float3 c = Floor(pos.xy, solid);
    float depth = -dot(float4(c, 1), height);
    float3 light = CausticsLight(pos.xy, c) * CausticsFade(max(depth, 0));
    return solid ? float4(light, 0) : (float4)0;
}
)";

constexpr float kGiRadius = 32.0f;  // world units the bounce reaches
constexpr float kGiGamma = 2.2f;

constexpr int kAoConstants = 64;  // kViewHlsl's b1
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr float kAoRadius = 0.8f;    // world units, the engine's are metres
constexpr float kAoMaxSpan = 0.15f;  // of the view's height, the most one pixel's samples may cover
// RTAO: the world's opaque triangles streamed out of its draws as host clip x, y and z, built into one scene a frame
constexpr uint32_t kRayStride = 12;
constexpr uint64_t kRayVertices = 6ull << 20;  // 2M triangles, 72 MB
constexpr uint32_t kRayDraws = 4096;
constexpr uint32_t kRayCounter = 8;  // bytes of a draw's stream output counter
constexpr uint32_t kRayTransformBytes = 12 * sizeof(float);
// ray traced shadows: each shadow map's draws stream out their clip position and the interpolator an alpha test reads,
// divided into the map's uv and depth at its resolve and built into the scene its receivers trace
constexpr uint32_t kCasterStride = 32;
constexpr uint64_t kCasterRecords = 8ull << 20;  // 2.8M triangles a frame, 256 MB
constexpr uint32_t kCasterDraws = 16384;
constexpr uint32_t kCasterBytes = 32;     // a caster as kRayShadowPrelude reads it, then its lighting
constexpr uint32_t kCasterTextured = 8;   // compare bit: its fetch is a colour map global illumination reads
constexpr uint32_t kDivideChunk = 65535 * 64;  // records one dispatch divides at most
constexpr uint64_t kRayIdleFrames = 60;   // a map's scene is freed once unresolved this long
constexpr uint32_t kStreamRtao = 1;       // PipelineKey::stream_out
constexpr uint32_t kStreamCaster = 2;     // then kStreamCaster + 1 + n with interpolator n
constexpr float kRaySunSpread = 0.02f;    // tan of the sun's angular radius, as PCSS's
constexpr float kRayDisc = 2.0f;          // another light's disc radius, in stock texels of its map
constexpr float kStockSide = 1344.0f;     // a stock sun map; inside a vis area the maps are half

// a channel of 0xRRGGBB, 0 to 1
float Channel(uint32_t rgb, uint32_t shift) { return float(rgb >> shift & 0xFF) / 255.0f; }

// b1 of kAoHlsl: the draws map the title's ndc over the whole w x h target as d says, rect is the scissor
void OcclusionConstants(const OcclusionArgs& a, const DrawConstants& d, const D3D12_RECT& rect, uint32_t w,
                        uint32_t h, float c[kAoConstants]) {
    // a pixel centre's host ndc back through the viewport to the title's
    float nx = 2.0f / float(w) / d.ndc_scale[0];
    float ny = -2.0f / float(h) / d.ndc_scale[1];
    float ox = (-1 - d.ndc_offset[0]) / d.ndc_scale[0];
    float oy = (1 - d.ndc_offset[1]) / d.ndc_scale[1];
    // the host holds ndc depth * zs + zo
    float zs = d.ndc_scale[2];
    float zo = d.ndc_offset[2];
    float vw = float(rect.right - rect.left);
    float vh = float(rect.bottom - rect.top);
    const float out[kAoConstants] = {a.proj[0], a.proj[1], a.proj[2], a.proj[3],
                                     a.m32 * zs, zo - a.m22 * zs, 0, 0,
                                     nx, ny, ox, oy,
                                     float(rect.left), float(rect.top), vw, vh,
                                     kAoRadius, kAoMaxSpan * vh, 0, 0,
                                     a.fog_scale, a.fog_bias, 0, 0,
                                     Channel(a.fog_colour, 16), Channel(a.fog_colour, 8), Channel(a.fog_colour, 0), 0};
    std::memcpy(c, out, sizeof(out));
}

// dst = dst * src.a + src; the alpha the game's blends read stays
void OcclusionBlend(D3D12_RENDER_TARGET_BLEND_DESC& b) {
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlend = D3D12_BLEND_SRC_ALPHA;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

// dst = dst * (1 + src): light the scene's own colour takes, as what it falls on reflects it
void CausticsBlend(D3D12_RENDER_TARGET_BLEND_DESC& b) {
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_DEST_COLOR;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlend = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

// a depth resolve's formats
bool DepthFormat(xenos::TexFormat f) {
    return f == xenos::TexFormat::k_24_8 || f == xenos::TexFormat::k_24_8_FLOAT || f == xenos::TexFormat::k_32_FLOAT;
}

// a colour map's formats, not a normal map's two channels or a mask's one
bool AlbedoFormat(xenos::TexFormat f) {
    switch (f) {
        case xenos::TexFormat::k_1_5_5_5:
        case xenos::TexFormat::k_5_6_5:
        case xenos::TexFormat::k_8_8_8_8:
        case xenos::TexFormat::k_4_4_4_4:
        case xenos::TexFormat::k_DXT1:
        case xenos::TexFormat::k_DXT2_3:
        case xenos::TexFormat::k_DXT4_5:
        case xenos::TexFormat::k_8_8_8_8_AS_16_16_16_16:
        case xenos::TexFormat::k_DXT1_AS_16_16_16_16:
        case xenos::TexFormat::k_DXT2_3_AS_16_16_16_16:
        case xenos::TexFormat::k_DXT4_5_AS_16_16_16_16:
            return true;
        default:
            return false;
    }
}


// a draw RTAO streamed out: its vertices and the viewport FillDrawConstants gave it
struct RayDraw {
    uint64_t first = 0;
    uint32_t count = 0;
    float ndc_scale[3] = {};
    float ndc_offset[3] = {};
};

// a shadow map draw streamed out, kept until its target resolves or clears
struct CasterDraw {
    const Target* target = nullptr;
    uint32_t w = 0;  // the target's title pixels, which its ndc spans
    uint32_t h = 0;
    uint32_t count = 0;  // records
    uint32_t stream = 0;  // PipelineKey::stream_out
    bool alpha = false;
    uint32_t info[8] = {};  // kCasterBytes: first record, alpha view, alpha reference, compare and uv components,
                            // then its material colour 0xRRGGBB, 1 when the sun lights it and its ambient
    bool view = false;      // a camera pass draw for global illumination, divided into view space
    float ndc[4] = {};      // its ndc scale and offset over x and y
};

// records' clip positions divided into a map's uv and depth: first record, count, then the uv scale and offset over
// ndc x and y. In view mode the camera's view space instead, x and y scaled and offset by w, which is the distance
const char* kDivideHlsl = R"(
RWByteAddressBuffer records : register(u0);
cbuffer C : register(b0) {
    uint first;
    uint count;
    float2 scale;
    float2 offset;
    uint view;
};
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID) {
    if (id >= count) return;
    uint at = (first + id) * 32;
    float4 p = asfloat(records.Load4(at));
    // a vertex at or behind the light leaves its triangle out
    float3 q = p.w > 0 ? float3(p.xy / p.w * scale + offset, p.z / p.w) : asfloat(0x7FC00000u).xxx;
    if (view) q = float3(p.xy * scale + offset * p.w, p.w);
    records.Store3(at, asuint(q));
}
)";
constexpr uint32_t kDivideConstants = 7;

// a caster's clip position, then the interpolator its alpha test reads or nothing
D3D12_STREAM_OUTPUT_DESC CasterOutput(uint32_t stream) {
    static const auto kDecls = [] {
        std::array<std::array<D3D12_SO_DECLARATION_ENTRY, 2>, 17> d{};
        for (uint32_t i = 0; i < d.size(); i++) {
            d[i][0] = {0, "SV_Position", 0, 0, 4, 0};
            d[i][1] = {0, i ? "TEXCOORD" : nullptr, i ? i - 1 : 0, 0, 4, 0};
        }
        return d;
    }();
    static const UINT kStrides[] = {kCasterStride};
    return {kDecls[stream - kStreamCaster].data(), 2, kStrides, 1, 0};
}

// a draw's host clip x, y and z to the view space kAoHlsl's View() makes: the title's z is m32 - m22 * w (823B0580
// flips it to w - z), so the distance w comes back from z alone
void RayTransform(const OcclusionArgs& a, const RayDraw& d, uint8_t* out) {
    float q = 1 / (d.ndc_offset[2] - d.ndc_scale[2] * a.m22);
    float w0 = -q * d.ndc_scale[2] * a.m32;  // w = q * z + w0
    float bx = -a.proj[0] * (d.ndc_offset[0] / d.ndc_scale[0] + a.proj[2]);
    float by = -a.proj[1] * (d.ndc_offset[1] / d.ndc_scale[1] + a.proj[3]);
    const float rows[12] = {a.proj[0] / d.ndc_scale[0], 0, bx * q, bx * w0,
                            0, a.proj[1] / d.ndc_scale[1], by * q, by * w0,
                            0, 0, q, w0};
    std::memcpy(out, rows, sizeof(rows));
}

// triangles a strip of count indices draws, restarting at all ones indices when cut; byte order keeps all ones
uint32_t StripTriangles(const DrawArgs& a, uint32_t count, bool cut) {
    if (!cut || !a.indices) return count > 2 ? count - 2 : 0;
    uint32_t size = a.index32 ? 4 : 2;
    uint32_t triangles = 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < a.count; i++) {
        const uint8_t* p = a.indices + i * size;
        bool restart = p[0] == 0xFF && p[1] == 0xFF && (size == 2 || (p[2] == 0xFF && p[3] == 0xFF));
        run = restart ? 0 : run + 1;
        if (run >= 3) triangles++;
    }
    return triangles;
}

// vertices a triangle draw streams out, none for other primitives
uint32_t TriangleVertices(const DrawArgs& a, D3D12_PRIMITIVE_TOPOLOGY topo, uint32_t count, bool cut) {
    uint32_t triangles = 0;
    if (topo == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) triangles = count / 3;
    if (topo == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) triangles = StripTriangles(a, count, cut);
    return triangles * 3;
}

// the motion vectors the scene's draws left, with the depth reprojected from last frame's camera where none did.
// reproject: a title clip position to last frame's; ndc: a pixel centre to the title's ndc; zuv: z = (raw - y) / x,
// then the title's ndc to the target's uv
const char* kMotionHlsl = R"(
cbuffer M : register(b1) {
    row_major float4x4 reproject;
    float4 ndc;
    float4 zuv;
};
Texture2D<float> depth : register(t0);
Texture2D<float2> drawn : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 motion(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    float2 m = drawn.Load(p);
    if (m.x < 16384) return m;
    float2 n = pos.xy * ndc.xy + ndc.zw;
    float4 last = mul(float4(n, (depth.Load(p) - zuv.y) / zuv.x, 1), reproject);
    float2 mv = (last.xy / last.w - n) * zuv.zw;
    return last.w > 0 ? clamp(mv, -2, 2) : 2;
}
)";

// the radical inverse of i in base b, 0 to 1
float Halton(uint32_t i, uint32_t b) {
    float f = 1;
    float r = 0;
    for (; i; i /= b) {
        f /= float(b);
        r += f * float(i % b);
    }
    return r;
}

// b5 of a motion draw
struct MotionConstants {
    float reproject[16];
    uint32_t prev;  // run over the previous constants, else reproject
    uint32_t unused[3];
};

const char* kBlitHlsl = R"(
cbuffer P : register(b1) { float4 uvrect; float4 opts; };
Texture2D<float4> img : register(t0);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 blit(O i) : SV_Target {
    int n = max(int(opts.z), 1);
    float2 px = fwidth(i.uv);
    float4 c = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    float3 l = saturate(c.rgb);
    if (opts.x > 0) c.rgb = lerp(1.055 * pow(l, 1 / 2.4) - 0.055, l * 12.92, step(l, 0.0031308));
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

uint64_t ResolvedKey(uint32_t base, uint64_t key) { return uint64_t(base) | key << 32; }

constexpr uint32_t kPitchAlign = 80;         // EDRAM tile width a guest pitch rounds up to
constexpr uint64_t kShadowIdleFrames = 120;  // shadow frames a shadow map may go unused before it is freed
constexpr uint64_t kSideGrace = 8;           // presents the previous shadow side is still drawn after a change

// XEX_PERF: what one second of frames cost, logged once a second
struct Perf {
    uint32_t frames = 0;
    uint32_t draws = 0;
    uint32_t clears = 0;
    uint32_t resolves = 0;
    uint32_t tex_uploads = 0;
    uint64_t tex_bytes = 0;
    uint64_t upload_bytes = 0;
    uint32_t shaders = 0;
    uint32_t pipelines = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
Perf g_perf;

void LogPerf() {
    Perf& p = g_perf;
    p.frames++;
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.start).count();
    if (secs < 1) return;
    double n = p.frames;
    constexpr double kMb = 1048576.0;
    LOGR("perf: %.1f fps, %.2f ms/frame | per frame %.0f draws, %.1f resolves, %.1f clears, %.1f texture uploads "
         "%.2f MB, %.2f MB upload ring | %u shaders, %u pipelines made",
         n / secs, secs * 1000 / n, p.draws / n, p.resolves / n, p.clears / n, p.tex_uploads / n,
         p.tex_bytes / n / kMb, p.upload_bytes / n / kMb, p.shaders, p.pipelines);
    p = Perf{};
}

// BMP: BITMAPFILEHEADER then BITMAPINFOHEADER, 32 bit top down
constexpr uint32_t kBmpHeaderBytes = 54;
constexpr uint32_t kBmpInfoBytes = 40;

void WriteBmp(const std::filesystem::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgra) {
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int at, uint32_t v) { std::memcpy(hd + at, &v, 4); };
    put(2, kBmpHeaderBytes + uint32_t(bgra.size()));
    put(10, kBmpHeaderBytes);
    put(14, kBmpInfoBytes);
    put(18, w);
    put(22, uint32_t(-int32_t(h)));
    hd[26] = 1;   // planes
    hd[28] = 32;  // bits per pixel
    put(34, uint32_t(bgra.size()));
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(hd), kBmpHeaderBytes);
    f.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
}

// runs a job over an index range on the workers, with the caller (Run) or in the background (Start, then Wait)
class WorkPool {
public:
    WorkPool() {
        uint32_t workers = std::max(1u, std::thread::hardware_concurrency() / 2) - 1;
        for (uint32_t i = 0; i < workers; i++) threads_.emplace_back([this] { Work(); });
    }
    ~WorkPool() {
        {
            std::lock_guard lock(lock_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : threads_) t.join();
    }

    size_t Workers() const { return threads_.size(); }

    void Run(size_t count, const std::function<void(size_t)>& job) {
        Start(count, job);
        Take();
        Wait();
    }
    // job lives until Wait returns
    void Start(size_t count, const std::function<void(size_t)>& job) {
        Wait();
        if (!count) return;
        {
            std::lock_guard lock(lock_);
            job_ = &job;
            count_ = count;
            next_ = 0;
            busy_ = threads_.size();
            generation_++;
        }
        wake_.notify_all();
    }
    void Wait() {
        std::unique_lock lock(lock_);
        done_.wait(lock, [&] { return busy_ == 0; });
    }

private:
    void Take() {
        for (size_t i = next_++; i < count_; i = next_++) (*job_)(i);
    }

    void Work() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(lock_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            Take();
            std::lock_guard lock(lock_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(size_t)>* job_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_ = 0;
    size_t busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

// a texture view as the draw needs it; Texture pointers live as long as the backend
struct ViewKey {
    const Texture* tex = nullptr;
    uint32_t dimension = 0;
    uint32_t mapping = 0;
    uint32_t fits = 0;
    uint32_t unused = 0;  // keeps the hashed bytes defined
    bool operator==(const ViewKey&) const = default;
};

struct ViewKeyHash {
    size_t operator()(const ViewKey& k) const { return size_t(Hash64(&k, sizeof(k))); }
};

struct FetchHash {
    size_t operator()(const std::array<uint32_t, 6>& f) const { return size_t(Hash64(f.data(), sizeof(f))); }
};

class D3D12Backend final : public Backend, public Context {
public:
    ~D3D12Backend() override;
    bool Init(uint32_t width, uint32_t height) override;
    Shader* CreateShader(ShaderStage stage, const uint32_t* code, size_t dwords) override;
    const ShaderInfo& Info(const Shader* s) const override { return s->info; }
    void Draw(const State& s, const DrawArgs& a) override;
    void Clear(const State& s, uint32_t flags, const float color[4], float z, uint32_t stencil,
               const Rect* rect) override;
    void Resolve(const State& s, const ResolveArgs& r) override;
    void Copy(const CopyArgs& c) override;
    void Present(const uint32_t front[6]) override;
    void BeginQuery(uint32_t query) override;
    void EndQuery(uint32_t query) override;
    bool QueryResult(uint32_t query, uint32_t& samples) override;

    // Context
    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_.Get(); }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Rtv(t.view); }
    void Unbind() override;
    ID3D12Resource* Feedback(Target& t) override;
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps,
              float mul, DXGI_FORMAT rtv) override;
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override {
        Pass(hlsl, "vs", entry, rtv, rh, w, h, src, fmt, p, blend);
    }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    bool CreateDevice();
    bool CreateSwapChain();
    void CreateHeaps();
    bool CreateRootSignature();
    void CreateFrames();
    void LoadPipelineCache();
    void StorePipeline(const PipelineKey& k, D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                       ID3D12PipelineState* pso);

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    void Begin();
    void Submit(bool wait);
    void Kick();
    void WaitFence(uint64_t value);
    Alloc Upload(uint64_t size, uint64_t align);
    D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const void* data, uint32_t size);
    uint32_t AllocViews(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCpu(uint32_t i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGuest(uint32_t i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE CopyView(uint32_t i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE TextureView(uint32_t i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(uint32_t i) const;

    // targets
    uint32_t TargetHeight(const State& s) const;
    bool TargetKeyOf(const Surface& sf, bool depth, uint32_t width, uint64_t& map_key) const;
    Target* EnsureTarget(const Surface& sf, bool depth, uint32_t width, uint32_t height);
    void DropTargets();
    Target* ColorTarget(const State& s, uint32_t i, uint32_t height);
    Target* DepthTarget(const State& s, uint32_t height);
    uint32_t SpanTiles(const Target& t) const;
    void ClearTarget(Target& t, uint32_t flags, const float color[4], float z, uint8_t stencil, const D3D12_RECT* rects,
                     uint32_t count);
    void ClearSpan(const Target& cleared, uint32_t flags, const float color[4], float z, uint8_t stencil);

    // shaders and pipelines
    Variant* EnsureVariant(Shader* s, const ShaderOptions& opt);
    Variant* FindVariant(Shader* s, const ShaderOptions& opt);
    ID3D12PipelineState* EnsurePipeline(const PipelineKey& k, const Variant& vs, const Variant* ps);
    ID3D12PipelineState* EnsureDepthPass(const State& s, const ShaderOptions& ps_opt, const Variant& vs,
                                         PipelineKey& k);
    ID3D12PipelineState* FindPipeline(const PipelineKey& k, const Variant& vs, const Variant* ps);
    ComPtr<ID3D12PipelineState> MakePipeline(const BuiltPipeline& b) const;
    void TakeBuilt();
    bool Async() const { return app::Get().async_shaders && !cap_; }
    void FillPipelineDesc(const PipelineKey& k, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    ID3D12PipelineState* EnsureResolvePipeline(DXGI_FORMAT rtv, bool ms, bool filter = false);
    uint32_t Samples(uint32_t want);
    uint32_t HostSamples(uint32_t title) const { return title > 1 ? msaa_ : title; }

    // textures
    Resolved* FindResolved(const xenos::TextureFetch& t, const TextureBinding& b, ShaderStage stage, uint32_t slot);
    void TexFlags(const State& s, const ShaderInfo& info, ShaderStage stage, uint8_t flags[32]);
    bool SunCascade(const State& s, const ShaderInfo& info, ShaderOptions& opt);
    bool OceanFlags(const State& s, const ShaderInfo& info, ShaderOptions& opt);
    bool FogFlags(const State& s, ShaderOptions& opt);
    bool CausticsFlags(const State& s, const ShaderInfo& info, ShaderOptions& opt);
    uint8_t SoftShadow(uint32_t base, bool far_map) const;
    Resolved* SunMap(uint32_t base);
    void SunMapView(Resolved& r, D3D12_CPU_DESCRIPTOR_HANDLE at);
    void FillTextureView(const TextureBinding& b, const uint32_t fetch[6], D3D12_CPU_DESCRIPTOR_HANDLE at,
                         ShaderStage stage, uint32_t slot);
    Texture* EnsureTexture(const xenos::TextureFetch& t);
    Texture* FrameTexture(const uint32_t fetch[6], const xenos::TextureFetch& t);
    void HashAhead();
    uint64_t NextHash(Texture& tex);
    void UploadTexture(Texture& tex, const xenos::TextureFetch& t);
    void UploadRows(ID3D12Resource* res, uint32_t sub, const std::vector<uint8_t>& rows, uint32_t pitch);
    void LatchPackKey(Texture& tex, const xenos::TextureFetch& t);
    void LatchPackImage(Texture& tex);
    void CreatePackTexture(Texture& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    D3D12_SAMPLER_DESC SamplerDesc(const TextureBinding& b, const uint32_t fetch[6]) const;
    D3D12_GPU_DESCRIPTOR_HANDLE EnsureSamplers(const std::vector<D3D12_SAMPLER_DESC>& descs);
    D3D12_GPU_DESCRIPTOR_HANDLE LinearSamplers();
    uint32_t BindTextures(const State& s, const ShaderInfo& info, ShaderStage stage, uint32_t first, uint32_t slots,
                          D3D12_GPU_DESCRIPTOR_HANDLE& samplers);
    void NullViews(uint32_t first, uint32_t count);
    void BindViews(const State& s, const ShaderInfo& vs, const ShaderInfo& ps);

    // draws
    struct Bound {
        uint32_t w = 0;  // title pixels
        uint32_t h = 0;
        float scale = 1;
        Target* rt = nullptr;  // the first colour target
        Target* ds = nullptr;
        bool motion = false;  // the motion target bound at kMotionTarget
        bool albedo = false;  // the albedo target bound at fcip::kAlbedoTarget
    };
    bool BindTargets(const State& s, const ShaderInfo& ps, PipelineKey& k, Bound& b, bool motion = false,
                     bool albedo = false);
    void FillDrawConstants(const State& s, const Bound& b, DrawConstants& c) const;
    Rect GuestScissor(const State& s, uint32_t w, uint32_t h) const;
    D3D12_RECT HostRect(const Rect& r, float scale) const;
    bool UploadIndices(const DrawArgs& a, D3D12_PRIMITIVE_TOPOLOGY& topo, uint32_t& count, uint32_t& min,
                       uint32_t& max);
    bool UploadDirectIndices(const DrawArgs& a, uint32_t& min, uint32_t& max);
    D3D12_GPU_VIRTUAL_ADDRESS UploadStreams(const DrawArgs& a, const ShaderInfo& vs, uint32_t min, uint32_t max);
    void BindConstants(const State& s, D3D12_GPU_VIRTUAL_ADDRESS fetch, DrawConstants& c, const Target* rt,
                       uint32_t src_blend);
    void BindFloats(uint32_t root, const uint8_t* be, const uint64_t (&read)[2]);
    void FillKeyStates(const State& s, PipelineKey& k) const;

    // present
    void ResizeSwap();
    void UpdateHdrDisplay();
    bool ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space);
    bool EnsureOutTargets();
    bool CreateOutTarget(int i, DXGI_FORMAT fmt);
    void DrawFront(const xenos::TextureFetch& t, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE target);
    void BlitToSwap(ID3D12Resource* src, D3D12_RESOURCE_STATES& state, DXGI_FORMAT fmt, DXGI_FORMAT rtv,
                    D3D12_CPU_DESCRIPTOR_HANDLE target, UINT mapping, uint32_t w, uint32_t h);
    void RenderUi();
    void ShowFrame(const xenos::TextureFetch& t);
    void PresentHdr(const xenos::TextureFetch& t, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    ID3D12Resource* Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    struct SmaaLookup {
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        uint32_t bpp;  // bytes per texel
        const uint8_t* data;
    };
    bool EnsureSmaaLookups();
    bool CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l);
    bool EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool CreateSmaaTarget(int i, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl);
    D3D12_CPU_DESCRIPTOR_HANDLE SmaaView(int i) { return Rtv(kSmaaRtv + i); }
    // one full screen pass of kAoHlsl, t0 the depth, t1 src, t2 RTAO's scene
    struct AoPass {
        const char* entry;
        DXGI_FORMAT out = kAoFormat;
        uint32_t samples = 1;  // of the target drawn to
        bool blend = false;    // OcclusionBlend
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        ID3D12Resource* src = nullptr;
        float step_x = 0;
        float step_y = 0;
        D3D12_GPU_VIRTUAL_ADDRESS scene = 0;
        DXGI_FORMAT src_format = kAoFormat;
        bool rgb = false;  // colour written alone, the alpha kept
        bool gi = false;   // kGiHlsl's, with its views from t3
        ID3D12Resource* colour = nullptr;  // the scene kGiHlsl reads, a copy
        DXGI_FORMAT colour_format = DXGI_FORMAT_UNKNOWN;
        bool fog = false;        // kFogHlsl's, the sun map at t2
        Resolved* sun = nullptr;  // the far sun map, or the fog's own
        ID3D12Resource* shade = nullptr;  // the fog's terrain shade
        bool clouds = false;      // kCloudHlsl's
        bool half = false;        // drawn to a target of half the depth's size
        bool caustics = false;    // kCausticsHlsl's, the sun map at t2
        bool lighten = false;     // CausticsBlend
    };
    void Occlusion(const State& s, const OcclusionArgs& a);
    void BeginRayScene();
    void VolumetricFog(const State& s, const OcclusionArgs& a);
    bool EnsureFogTargets(uint32_t w, uint32_t h);
    ID3D12Resource* ShadeTexture();
    void VolumetricClouds(const State& s, const OcclusionArgs& a);
    bool EnsureCloudTarget(uint32_t w, uint32_t h);
    void Caustics(const State& s, const OcclusionArgs& a);
    bool EnsureCausticsTarget(uint32_t w, uint32_t h);

    // global illumination of the camera pass: its draws' albedo beside the colour, its world's triangles traced
    void BeginIllumination(const State& s);
    void Illumination(const State& s, const OcclusionArgs& a);
    bool EnsureIlluminationTargets(const Target& rt);
    uint32_t LightW() const { return (gi_.w + 1) / 2; }
    uint32_t LightH() const { return (gi_.h + 1) / 2; }
    bool AlbedoWanted(const State& s, const ShaderInfo& ps) const;
    void AlbedoFlags(const State& s, const ShaderInfo& ps, ShaderOptions& opt) const;
    bool SceneCaster(const PipelineKey& k, const Bound& b) const;
    bool FirstTextureAlbedo(const State& s) const;
    uint32_t MaterialColour(const State& s) const;
    uint32_t AmbientColour(const State& s) const;
    bool SunReceiver(const State& s) const;
    void DivideView(const OcclusionArgs& a);
    void TargetView(ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t samples, uint32_t slot);
    void CapIllumination(const State& s, const PipelineKey& k, const Bound& b, const CasterDraw& caster);

    // motion vectors of the camera pass's world, for FSR
    void BeginMotion(const State& s);
    void EndMotion(const State& s);
    bool EnsureMotionTargets(uint32_t w, uint32_t h);
    bool MotionWanted(const State& s, const ShaderInfo& ps) const;
    void BindMotion(const State& s, const DrawArgs& a, const ShaderInfo& vs);
    void CompleteMotion(Target& depth, const DrawConstants& d);
    void CapMotionMatch(const uint64_t (&read)[2], const uint8_t* now_be, const uint8_t* last, bool placed);

    // FSR: the camera's pass at the scene scale, upscaled at its end into its target at the render scale
    bool Fsr() const { return fcip::Get().fsr && fsr::Upscaling(); }
    // frame generation's swap chain only while it is on: it costs a copy and two threads every present
    bool GenerationWanted() const { return fcip::Get().frame_generation && Fsr() && fsr::FrameGeneration(); }
    void RecreateSwapChain();
    void ConfigureGeneration(bool enabled);
    void KeepHudless(const State& s);
    bool EnsureHudless();
    void DrawHudless(const xenos::TextureFetch& t);
    bool SceneScaled(uint32_t width) const { return upscale_.scene && width == guest_w_; }
    uint64_t MapKey(const Target& t) const;
    float SceneScale() const;
    float BaseScale() const;
    void BeginScene();
    void EndScene();
    bool EnsureUpscaled(uint32_t w, uint32_t h);
    void DrawUpscaled(ID3D12Resource* src, D3D12_RESOURCE_STATES& state, DXGI_FORMAT fmt, uint32_t taps, Target& out);
    bool DrawSharpened(Target& scene, Target& out);
    uint32_t RayVertices(const DrawArgs& a, const PipelineKey& k, D3D12_PRIMITIVE_TOPOLOGY topo, uint32_t count,
                         bool alpha_test) const;
    void StreamOut(const DrawConstants& c, uint32_t vertices);
    D3D12_GPU_VIRTUAL_ADDRESS BuildRayScene(const OcclusionArgs& a);
    bool BuildStructure(ID3D12GraphicsCommandList4* cl4, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in,
                        ComPtr<ID3D12Resource>& dst);
    bool Grow(ComPtr<ID3D12Resource>& r, uint64_t bytes, D3D12_RESOURCE_STATES state);
    bool MakeBuffer(ComPtr<ID3D12Resource>& r, uint64_t bytes, D3D12_RESOURCE_STATES state);
    bool CreateDividePipeline();
    bool EnsureCasterBuffers();
    uint32_t CasterVertices(const State& s, const DrawArgs& a, const PipelineKey& k, D3D12_PRIMITIVE_TOPOLOGY topo,
                            uint32_t count, const Bound& b, bool alpha_test, CasterDraw& caster);
    void StreamOutCasters(CasterDraw caster, const DrawConstants& c);
    void DropCasters(const Target* t);
    void BuildRayShadow(const Target& src, const ResolveArgs& r);
    void Stamp(uint8_t kind);  // diag
    void EndStamps();          // diag
    void ReadStamps();         // diag
    bool BuildCasterScene(const std::vector<CasterDraw>& draws, bool opaque, ComPtr<ID3D12Resource>& bottom_out,
                          ComPtr<ID3D12Resource>& top_out);
    void DivideCasters(const std::vector<CasterDraw>& draws, const ResolveArgs& r);
    void BeginDivide();
    void DivideRecords(uint32_t first, uint32_t count, const float scale_offset[4], uint32_t view);
    void EndDivide();
    void RayShadows(const State& s, const ShaderInfo& info, ShaderOptions& opt);
    void RayViews(uint32_t table, const D3D12_GPU_VIRTUAL_ADDRESS* rays);
    void CasterViews(uint32_t at);
    float RaySpread(uint32_t base, const fcip::SunMaps& sun) const;
    // buffers are back in COMMON once a list ran
    void BuffersDecayed() {
        ray_vertices_state_ = D3D12_RESOURCE_STATE_COMMON;
        ray_counters_state_ = D3D12_RESOURCE_STATE_COMMON;
        caster_records_state_ = D3D12_RESOURCE_STATE_COMMON;
        caster_counters_state_ = D3D12_RESOURCE_STATE_COMMON;
        casters_state_ = D3D12_RESOURCE_STATE_COMMON;
    }
    bool EnsureOcclusionTargets(uint32_t w, uint32_t h);
    ID3D12PipelineState* EnsureOcclusionPipeline(const AoPass& p, bool ms_depth);
    bool OcclusionPass(const AoPass& p, Target& depth, float c[kAoConstants], const D3D12_RECT& rect);
    void CopyResolve(Target& src, Resolved& dst, const ResolveArgs& r);
    Resolved& EnsureResolved(uint64_t key, DXGI_FORMAT format, uint32_t w, uint32_t h, float scale);
    void MakeResolved(Resolved& dst, DXGI_FORMAT format, uint32_t w, uint32_t h, float scale);
    void ReleaseResolved(Resolved& r);
    void DropResolved(uint32_t base);
    void DropTarget(uint64_t map_key);
    bool ShadowWidth(uint32_t width) const;
    void KeepShadowTarget(const Target& t);
    void DropIdleShadowMaps();
    void KeepBlocks(Target& src, Resolved& dst, const ResolveArgs& r);
    Resolved* CpuSource(uint32_t base);
    bool MakeCpuTexels(Resolved& from);
    bool CopyCpuTexels(ComPtr<ID3D12Resource>& buffer, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp);
    void RecordReadBack(ReadBack& rb, uint32_t base, uint64_t seq);
    void WriteResolved(const uint32_t fetch[6]);

    // passes
    // Pass with its own vertex shader entry; false when its pipeline cannot be made
    bool Pass(const char* hlsl, const char* vs_entry, const char* entry, DXGI_FORMAT rtv,
              D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w, uint32_t h, ID3D12Resource* const src[3],
              const DXGI_FORMAT fmt[3], const float p[8], float blend);
    ID3D12PipelineState* EnsurePassPipeline(const std::string& hlsl, const char* vs_entry, const char* entry,
                                            DXGI_FORMAT rtv, float blend);
    void ShaderView(ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t slot);
    void FullScreen(ID3D12PipelineState* pso, uint32_t views, const float p[8]);

    // F3 capture under XEX_CAPTURE_DIR, else captures beside the exe: the next frame's draws listed, each target
    // saved as the frame leaves it
    void StartCapture();
    void FinishCapture(const xenos::TextureFetch& front);
    void CapStep(Target* rt);
    void CapTextures(const State& s, const ShaderInfo& ps);
    void CapSave(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, DXGI_FORMAT view, uint32_t samples, bool depth,
                 uint32_t w, uint32_t h, float mul, const std::string& name,
                 UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    void CapSave(Target& t, const std::string& name, float mul);
    void CapFinite(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, const char* what);
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    Target* cap_rt_ = nullptr;

    ComPtr<IDXGIFactory4> factory_;
    // HDR10 output: the window's display, the output asked and in use, the frame and panel targets it draws first
    HMONITOR hdr_mon_ = nullptr;
    bool hdr_display_ = false;
    bool hdr_want_ = false;
    bool hdr_out_ = false;
    float peak_nits_ = kDefaultPeakNits;
    float paper_nits_ = kDefaultPaperNits;
    ComPtr<ID3D12Resource> out_[kOutTargets];
    // SMAA: edges, blend weights and the blended frame, at the frame's size; its area and search lookups
    ComPtr<ID3D12Resource> smaa_[kSmaaTargets];
    ComPtr<ID3D12Resource> smaa_lut_[2];
    D3D12_RESOURCE_STATES smaa_state_[kSmaaTargets] = {};
    uint32_t smaa_w_ = 0;
    uint32_t smaa_h_ = 0;
    DXGI_FORMAT smaa_fmt_ = DXGI_FORMAT_UNKNOWN;
    // ambient occlusion: the raw pass and its first blur, each with view distance beside it
    ComPtr<ID3D12Resource> ao_[kAoTargets];
    D3D12_RESOURCE_STATES ao_state_[kAoTargets] = {};
    uint32_t ao_w_ = 0;
    uint32_t ao_h_ = 0;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> ao_pipelines_;
    // volumetric fog: its march, later its medium, and its first blur
    ComPtr<ID3D12Resource> fog_[kFogTargets];
    D3D12_RESOURCE_STATES fog_state_[kFogTargets] = {};
    uint32_t fog_w_ = 0;
    uint32_t fog_h_ = 0;
    // past the fog's pass until kClearFogEnd, its medium in fog_[0], read by draws into its target
    bool fog_near_ = false;
    uint64_t fog_rt_key_ = 0;
    ID3D12Resource* fog_view_ = nullptr;  // the medium the draw's pixel shader reads at fcip::kFogSlot
    // volumetric clouds' march at half size, grown to the largest pass so far
    ComPtr<ID3D12Resource> clouds_;
    D3D12_RESOURCE_STATES clouds_state_ = D3D12_RESOURCE_STATE_COMMON;
    uint32_t clouds_w_ = 0;
    uint32_t clouds_h_ = 0;
    // the caustics' light, past its pass read by the water's draws into its target at fcip::kCausticsSlot
    ComPtr<ID3D12Resource> caustics_;
    D3D12_RESOURCE_STATES caustics_state_ = D3D12_RESOURCE_STATE_COMMON;
    uint32_t caustics_w_ = 0;
    uint32_t caustics_h_ = 0;
    bool caustics_lit_ = false;    // drawn since the last kClearCaustics
    bool caustics_water_ = false;  // between kClearCausticsWater and kClearCausticsDry
    uint64_t caustics_rt_key_ = 0;
    ID3D12Resource* caustics_view_ = nullptr;  // the light the draw's pixel shader reads, null for none
    // the terrain shade as last published
    ComPtr<ID3D12Resource> shade_;
    D3D12_RESOURCE_STATES shade_state_ = D3D12_RESOURCE_STATE_COMMON;
    uint32_t shade_generation_ = 0;
    // motion vectors: as the scene's draws wrote them, then completed by reprojecting the depth where none did
    struct Motion {
        bool open = false;    // between kClearMotionBegin and the scene's end
        bool active = false;  // open and not paused
        uint64_t rt_key = 0;  // the scene's colour target
        ComPtr<ID3D12Resource> drawn;
        ComPtr<ID3D12Resource> done;
        D3D12_RESOURCE_STATES drawn_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES done_state = D3D12_RESOURCE_STATE_COMMON;
        uint32_t w = 0;
        uint32_t h = 0;
        MotionCamera camera;
        float last_clip[16] = {};
        bool has_last = false;
        float reproject[16] = {};
        D3D12_GPU_VIRTUAL_ADDRESS consts[2] = {};  // b5: reprojected, then run over the previous constants
        uint32_t draws = 0;
        uint32_t matched = 0;
    } motion_;
    MotionHistory motion_history_;
    // global illumination: the world's draws streamed into casters, the albedo its draws wrote, the traced light
    struct Gi {
        bool capture = false;  // the camera pass's world streams out, from kClearIlluminationScene to its world's end
        uint32_t mode = 0;     // the albedo draws take, kAlbedo*, 0 outside the camera's pass
        uint64_t rt_key = 0;   // the scene's colour target
        uint64_t ds_key = 0;   // and depth target, which depth only draws write alone
        float ambient[3] = {};  // the game's, as the world's end brought it
        std::vector<CasterDraw> draws;
        size_t divided = 0;  // draws in view space
        ComPtr<ID3D12Resource> bottom;
        ComPtr<ID3D12Resource> top;
        ComPtr<ID3D12Resource> albedo;
        D3D12_RESOURCE_STATES albedo_state = D3D12_RESOURCE_STATE_COMMON;
        ComPtr<ID3D12Resource> light[kGiTargets];  // traced, then blurred across
        D3D12_RESOURCE_STATES light_state[kGiTargets] = {};
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t samples = 0;
    } gi_;
    // FSR: the scene's targets and camera as EndMotion left them, the upscaled frame
    struct Upscale {
        bool scene = false;   // between kClearSceneBegin and kClearSceneEnd
        bool motion = false;  // EndMotion completed this scene's motion
        bool reset = true;    // nothing to carry over from the last upscale
        uint64_t color = 0;   // targets_ keys
        uint64_t depth = 0;
        MotionCamera camera;
        uint32_t jitters = 0;
        uint64_t configured = ~0ull;  // the frame frame generation was last set up for
        ComPtr<ID3D12Resource> out;
        D3D12_RESOURCE_STATES out_state = D3D12_RESOURCE_STATE_COMMON;
        uint32_t w = 0;
        uint32_t h = 0;
    } upscale_;
    // frame generation tells the HUD apart by the frame without it, in the back buffer's form
    struct Hudless {
        ComPtr<ID3D12Resource> frame;  // the target as the post effects left it, before the HUD
        D3D12_RESOURCE_STATES frame_state = D3D12_RESOURCE_STATE_COMMON;
        DXGI_FORMAT frame_format = DXGI_FORMAT_UNKNOWN;
        uint64_t kept = ~0ull;  // the frame it was kept for
        // two: the swap chain may still generate from the last one while this frame draws the next
        ComPtr<ID3D12Resource> out[2];
        D3D12_RESOURCE_STATES out_state[2] = {};
        uint32_t w = 0;
        uint32_t h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t used = ~0ull;  // the frame frame generation was handed it for
    } hudless_;
    float scene_scale_ = 1;
    float jitter_[2] = {};  // host pixels the scene's draws are offset by
    uint32_t frame_rect_[4] = {};  // x, y, width and height of the frame in the back buffer
    // RTAO: inline ray tracing, null without; the world's streamed out triangles and their scene
    ComPtr<ID3D12Device5> dev5_;
    bool ray_capture_ = false;  // between OcclusionScene and Occlusion
    std::vector<RayDraw> ray_draws_;
    uint64_t ray_used_ = 0;  // vertices
    ComPtr<ID3D12Resource> ray_vertices_;
    ComPtr<ID3D12Resource> ray_counters_;
    D3D12_RESOURCE_STATES ray_vertices_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES ray_counters_state_ = D3D12_RESOURCE_STATE_COMMON;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> ray_geometry_;
    ComPtr<ID3D12Resource> ray_bottom_;
    ComPtr<ID3D12Resource> ray_top_;
    ComPtr<ID3D12Resource> ray_scratch_;
    // ray traced shadows: this frame's shadow map draws not yet resolved, and the scene each map's resolve built
    struct RayShadow {
        ComPtr<ID3D12Resource> bottom;
        ComPtr<ID3D12Resource> top;
        uint64_t frame = 0;
    };
    std::unordered_map<uint32_t, RayShadow> ray_shadows_;  // by map address
    bool fog_map_ = false;  // between kClearFogMap and kClearFogMapEnd
    // diag
    ComPtr<ID3D12QueryHeap> stamp_heap_;
    ComPtr<ID3D12Resource> stamp_read_;
    std::vector<uint8_t> stamp_kinds_[kFrames];
    uint8_t stamp_kind_ = kStampEnd;
    uint64_t stamp_hz_ = 0;
    double stamp_ms_[kStampEnd] = {};
    uint64_t stamp_records_ = 0;
    uint32_t stamp_refused_ = 0;
    uint32_t stamp_draws_[2] = {};
    uint32_t stamp_frames_ = 0;
    std::chrono::steady_clock::time_point stamp_start_ = std::chrono::steady_clock::now();
    std::vector<CasterDraw> caster_draws_;
    std::vector<CasterDraw> caster_build_;
    uint64_t casters_used_ = 0;   // records this frame
    uint32_t caster_slots_ = 0;   // draws this frame, a stream output counter each
    uint32_t casters_built_ = 0;  // casters this frame's scenes hold
    ComPtr<ID3D12Resource> caster_records_;
    ComPtr<ID3D12Resource> caster_counters_;
    ComPtr<ID3D12Resource> casters_;
    D3D12_RESOURCE_STATES caster_records_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES caster_counters_state_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES casters_state_ = D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12RootSignature> divide_root_;
    ComPtr<ID3D12PipelineState> divide_pso_;
    D3D12_GPU_VIRTUAL_ADDRESS ray_scenes_[fcip::kRayScenes] = {};  // the scenes the draw's pixel shader traces
    float ray_spread_[fcip::kRayScenes] = {};  // their lights' spread, as kRayShadowPrelude's RayShare takes it
    D3D12_RESOURCE_STATES out_state_[kOutTargets] = {};
    uint32_t out_w_ = 0, out_h_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain4> swap_;
    bool generation_wanted_ = false;  // GenerationWanted when swap_ was made
    bool generation_swap_ = false;    // swap_ is frame generation's
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12DescriptorHeap> view_heap_, sampler_heap_, rtv_heap_, dsv_heap_, ui_heap_;
    uint32_t view_inc_ = 0, sampler_inc_ = 0, rtv_inc_ = 0, dsv_inc_ = 0;
    ComPtr<ID3D12DescriptorHeap> copy_heap_;  // copied into tables: kPsTextures null views, then each frame's raw ring
    ComPtr<ID3D12DescriptorHeap> texture_view_heap_;
    std::unordered_map<ViewKey, uint32_t, ViewKeyHash> texture_views_;  // to their slot in texture_view_heap_
    std::vector<uint32_t> free_rtv_, free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    uint64_t presents_ = 0;
    bool tearing_ = false;
    bool helper_lanes_ = false;  // shader model 6.6: ray traced shadows skip the pixels drawn only for derivatives
    uint32_t swap_w_ = 0, swap_h_ = 0;
    uint32_t guest_w_ = 0, guest_h_ = 0;
    float scale_ = 1;
    uint32_t msaa_ = 1;      // host samples of the title's multisampled targets
    uint32_t msaa_set_ = 0;  // app::Settings::msaa they came from
    Extension* ext_ = nullptr;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint64_t, Texture> pack_textures_;  // by pack key, shared by the textures drawing them
    std::unordered_map<std::array<uint32_t, 6>, Texture*, FetchHash> frame_textures_;  // by fetch constant, this frame
    std::vector<Texture*> hash_ahead_;
    std::function<void(size_t)> hash_job_ = [this](size_t i) { TakeHash(*hash_ahead_[i]); };
    WorkPool pool_;  // after what its jobs use, so it stops first
    std::unordered_map<uint64_t, Resolved> resolved_;  // by ResolvedKey
    std::unordered_map<uint32_t, uint64_t> latest_;     // the ResolvedKey of the last resolve to each address
    std::unordered_map<uint32_t, Resolved> blocks_;     // by address: a resolve with the whole blocks it writes past
    Resolved* sun_[fcip::kSunMaps] = {};                // the sharper sun maps the draw's pixel shader reads
    fcip::SunMaps sun_maps_;
    fcip::OceanLook ocean_;  // the original Xbox ocean's look while OceanFlags flags a draw
    // shadow map targets and their resolves by the shadow frame that last used them and their guest width: a new side,
    // distance or vis area leaves some behind
    struct ShadowUse {
        uint64_t frame = 0;
        uint32_t width = 0;
    };
    std::unordered_map<uint64_t, ShadowUse> shadow_targets_;
    std::unordered_map<uint32_t, ShadowUse> shadow_resolves_;
    uint64_t shadow_frame_ = 0;  // frames that drew a shadow map
    bool shadow_drawn_ = false;
    uint32_t shadow_side_ = 0;       // SunShadowSide at the last present
    uint32_t last_shadow_side_ = 0;  // the one before, which frames recorded before the change still draw
    uint64_t side_changed_ = 0;      // the present it changed at
    std::unordered_map<uint64_t, std::array<float, 4>> pending_clears_;  // colour clears of targets not made yet
    Resolved cpu_texels_;                               // a resolve at title size, for the CPU
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pass_pipelines_;
    std::vector<std::unique_ptr<Shader>> shaders_;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines_;
    PipelineKey last_key_;  // the last draw's, found again without a hash
    ID3D12PipelineState* last_pso_ = nullptr;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> resolve_pipelines_;
    std::unordered_map<uint64_t, uint32_t> sampler_sets_;
    uint32_t samplers_used_ = 0;
    std::ofstream pipeline_cache_;  // each pipeline appended as it is made
    // async shaders: pipelines on the build threads and those they made
    std::unordered_set<uint64_t> building_;
    std::vector<BuiltPipeline> built_;
    std::mutex built_lock_;
    std::atomic<bool> built_ready_{false};
    // since the last log line
    uint32_t posted_shaders_ = 0;
    uint32_t posted_pipelines_ = 0;
    uint32_t skipped_draws_ = 0;

    // the last constant blocks bound, skipped when unchanged; no address means upload again
    uint8_t const_last_[kRootCount][kConstBytes] = {};
    D3D12_GPU_VIRTUAL_ADDRESS const_gpu_[kRootCount] = {};
    uint64_t const_held_[kRootCount][2] = {};  // the float constant blocks const_gpu_ holds, as ShaderInfo::constants

    // what the last draw set on the command list, set again only where a draw differs; Unbind forgets it
    struct Binds {
        bool valid = false;
        TargetViews targets;
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        float blend[4] = {};
        uint32_t stencil_ref = 0;
        ID3D12PipelineState* pso = nullptr;
        D3D12_PRIMITIVE_TOPOLOGY topology{};
        UINT64 roots[kRootCount] = {};  // constant buffers, view and sampler tables
        // the textures a stage's last draw bound, vertex then pixel
        struct Stage {
            const ShaderInfo* info = nullptr;  // null: none kept
            uint32_t aniso = 0;
            uint32_t fetch[kPsTextures][6] = {};
            uint32_t table = 0;
            D3D12_GPU_DESCRIPTOR_HANDLE samplers{};
            const Resolved* sun[fcip::kSunMaps] = {};
            D3D12_GPU_VIRTUAL_ADDRESS rays[fcip::kRayScenes] = {};
            ID3D12Resource* fog = nullptr;
            ID3D12Resource* caustics = nullptr;
        } stages[2];
    } binds_;
    // true where now differs from what the last draw set, which keeps it
    template <class T>
    bool Latch(T& last, const T& now) {
        bool same = binds_.valid && std::memcmp(&last, &now, sizeof(T)) == 0;
        std::memcpy(&last, &now, sizeof(T));
        return !same;
    }

    // per draw scratch, kept to save allocations
    std::vector<uint32_t> index_src_;
    std::vector<uint32_t> index_out_;
    std::vector<VertexFetchConstant> stream_consts_;
    std::vector<std::pair<std::pair<const uint8_t*, uint32_t>, uint32_t>> stream_copies_;
    std::vector<D3D12_SAMPLER_DESC> sampler_descs_;
    std::vector<KeptVertices> kept_vertices_ = std::vector<KeptVertices>(kKeptUploads);
    std::vector<KeptIndices> kept_indices_ = std::vector<KeptIndices>(kKeptUploads);
    uint64_t upload_epoch_ = 1;
    uint64_t draw_fences_ = 0;  // DrawArgs::fences of the last draw

    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_read_;
    Query queries_[kQueryCount];
    uint32_t open_queries_ = 0;

    std::chrono::steady_clock::time_point fps_start_ = std::chrono::steady_clock::now();
    uint32_t fps_frames_ = 0;
    double fps_ = 0;

};

D3D12Backend::~D3D12Backend() {
    if (!dev_) return;
    Submit(true);
    ImGui_ImplDX12_Shutdown();
    for (auto& b : bb_) b.Reset();
    if (generation_swap_) fsr::DestroySwapChain(swap_.Get());
    swap_.Reset();
    fsr::Shutdown();
    if (event_) CloseHandle(event_);
}

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    if (!CreateDevice() || !CreateSwapChain()) return false;
    CreateHeaps();
    if (!CreateRootSignature()) return false;
    if (dev5_) CreateDividePipeline();
    CreateFrames();
    LoadPipelineCache();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, kSwapFormat, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    scale_ = BaseScale();
    scene_scale_ = SceneScale();
    msaa_set_ = app::Get().msaa;
    msaa_ = Samples(msaa_set_);
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
    Begin();
    LOGR("D3D12: %ux%u title, %ux%u window, render scale %.3f", width, height, swap_w_, swap_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XEX_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    fsr::Load();
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // the dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("D3D12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    fsr::SetDevice(dev_.Get());
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    dev_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) dev_.As(&dev5_);
    g_ray_tracing = dev5_ != nullptr;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    helper_lanes_ = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                    sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    LOGR("D3D12: RTAO %s", dev5_ ? "available" : "unavailable, GTAO in its place");
    return true;
}

bool D3D12Backend::CreateSwapChain() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    swap_w_ = std::max<LONG>(rc.right - rc.left, 1);
    swap_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = swap_w_;
    sd.Height = swap_h_;
    sd.Format = kSwapFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain4* generated = nullptr;
    generation_wanted_ = GenerationWanted();
    generation_swap_ = generation_wanted_ && fsr::CreateSwapChain(factory_.Get(), queue_.Get(), hwnd, sd, &generated);
    ComPtr<IDXGISwapChain1> sc1;
    if (generation_swap_) {
        swap_.Attach(generated);
    } else if (SUCCEEDED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        sc1.As(&swap_);
    } else {
        LOGE("D3D12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    for (int i = 0; i < kFrames; i++) swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
    return true;
}

void D3D12Backend::CreateHeaps() {
    auto make = [&](D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = t;
        hd.NumDescriptors = n;
        hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
        return dev_->GetDescriptorHandleIncrementSize(t);
    };
    view_inc_ = make(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewsPerFrame * kFrames, true, view_heap_);
    sampler_inc_ = make(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, sampler_heap_);
    rtv_inc_ = make(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots, false, rtv_heap_);
    dsv_inc_ = make(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    make(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    make(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kPsTextures + kFrames, false, copy_heap_);
    make(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTextureViews, false, texture_view_heap_);
    D3D12_SHADER_RESOURCE_VIEW_DESC nv{};
    nv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nv.Texture2D.MipLevels = 1;
    for (uint32_t i = 0; i < kPsTextures; i++) dev_->CreateShaderResourceView(nullptr, &nv, CopyView(i));
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
    for (int i = 0; i < kFrames; i++) dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Rtv(kRtvCount + i));
    D3D12_RENDER_TARGET_VIEW_DESC null{};
    null.Format = kNullRtvFormat;
    null.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev_->CreateRenderTargetView(nullptr, &null, Rtv(kNullRtv));
}

// b0 to b3 as root constant buffers, then a texture table and a sampler table per stage
bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE vs_views[2] = {};
    vs_views[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vs_views[0].NumDescriptors = 1;
    vs_views[0].RegisterSpace = 1;
    vs_views[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vs_views[1].NumDescriptors = kVsTextures;
    vs_views[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_DESCRIPTOR_RANGE ps_views{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPsTextures, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE vs_samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kVsTextures, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE ps_samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kPsTextures, 0, 0, 0};

    D3D12_ROOT_PARAMETER p[kRootCount] = {};
    auto cbv = [&](uint32_t i, uint32_t reg, D3D12_SHADER_VISIBILITY vis) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[i].Descriptor.ShaderRegister = reg;
        p[i].ShaderVisibility = vis;
    };
    auto table = [&](uint32_t i, const D3D12_DESCRIPTOR_RANGE* r, uint32_t n, D3D12_SHADER_VISIBILITY vis) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[i].DescriptorTable.NumDescriptorRanges = n;
        p[i].DescriptorTable.pDescriptorRanges = r;
        p[i].ShaderVisibility = vis;
    };
    cbv(kRootVsConsts, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    cbv(kRootPsConsts, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    cbv(kRootFlow, 1, D3D12_SHADER_VISIBILITY_ALL);
    cbv(kRootDraw, 2, D3D12_SHADER_VISIBILITY_ALL);
    cbv(kRootFetch, 3, D3D12_SHADER_VISIBILITY_VERTEX);
    cbv(kRootPrevConsts, 4, D3D12_SHADER_VISIBILITY_VERTEX);
    cbv(kRootMotion, 5, D3D12_SHADER_VISIBILITY_VERTEX);
    table(kRootVsViews, vs_views, 2, D3D12_SHADER_VISIBILITY_VERTEX);
    table(kRootPsViews, &ps_views, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    table(kRootVsSamplers, &vs_samplers, 1, D3D12_SHADER_VISIBILITY_VERTEX);
    table(kRootPsSamplers, &ps_samplers, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_DESCRIPTOR_RANGE heap{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, 0};
    table(kRootHeap, &heap, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_STATIC_SAMPLER_DESC alpha{};
    alpha.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    alpha.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    alpha.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    alpha.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    alpha.MaxLOD = D3D12_FLOAT32_MAX;
    alpha.RegisterSpace = 3;
    alpha.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = dev5_ ? kRootCount : kRootHeap;
    rsd.pParameters = p;
    rsd.NumStaticSamplers = dev5_ ? 1 : 0;
    rsd.pStaticSamplers = &alpha;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;  // RTAO's world draws
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        LOGE("D3D12: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return SUCCEEDED(dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root_)));
}

void D3D12Backend::CreateFrames() {
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    for (Frame& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    // the vertex bytes: the whole upload ring, raw
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    raw.Buffer.NumElements = UINT(kUploadSize / 4);
    raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (int i = 0; i < kFrames; i++) {
        dev_->CreateShaderResourceView(frames_[i].upload.Get(), &raw, CopyView(kPsTextures + i));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    g_cpu.fence = fence_.Get();
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));

    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueryCount, 0};
    dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_));
    D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = BufferDesc(kQueryCount * sizeof(uint64_t));
    dev_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&query_read_));
    // diag
    D3D12_QUERY_HEAP_DESC sd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kFrames, 0};
    dev_->CreateQueryHeap(&sd, IID_PPV_ARGS(&stamp_heap_));
    D3D12_RESOURCE_DESC sr = BufferDesc(kStamps * kFrames * sizeof(uint64_t));
    dev_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &sr, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&stamp_read_));
    queue_->GetTimestampFrequency(&stamp_hz_);
}

std::filesystem::path PipelineCachePath() { return app::ExeDir() / "cache" / "pipelines.bin"; }

std::vector<char> ReadWhole(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<char> bytes(size_t(f.tellg()));
    f.seekg(0);
    f.read(bytes.data(), std::streamsize(bytes.size()));
    return bytes;
}

// the whole records; at is left past the last
std::vector<CachedPipeline> ParsePipelines(const std::vector<char>& file, size_t& at) {
    std::vector<CachedPipeline> cached;
    while (file.size() - at >= sizeof(PipelineRecord)) {
        PipelineRecord r;
        std::memcpy(&r, &file[at], sizeof(r));
        size_t size = sizeof(r) + size_t(r.vs_size) + r.ps_size + r.blob_size;
        if (file.size() - at < size) break;
        const char* p = &file[at + sizeof(r)];
        CachedPipeline c{r.key};
        c.vs = {p, r.vs_size};
        if (r.ps_size) c.ps = {p + r.vs_size, r.ps_size};
        c.blob = {p + r.vs_size + r.ps_size, r.blob_size};
        cached.push_back(c);
        at += size;
    }
    return cached;
}

// every pipeline earlier runs made is made again before the first frame, so none is made mid game; a driver change
// makes them from the DXIL and writes the file again
void D3D12Backend::LoadPipelineCache() {
    auto start = std::chrono::steady_clock::now();
    std::filesystem::path path = PipelineCachePath();
    std::vector<char> file = ReadWhole(path);
    const uint32_t header[2] = {kPipelineCacheMagic, sizeof(PipelineRecord)};
    size_t at = sizeof(header);
    bool known = file.size() >= at && std::memcmp(file.data(), header, at) == 0;
    std::vector<CachedPipeline> cached;
    if (known) cached = ParsePipelines(file, at);

    std::vector<ComPtr<ID3D12PipelineState>> made(cached.size());
    std::atomic<bool> stale = false;
    std::function<void(size_t)> make = [&](size_t i) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        FillPipelineDesc(cached[i].key, d);
        d.VS = cached[i].vs;
        d.PS = cached[i].ps;
        d.CachedPSO = cached[i].blob;
        if (SUCCEEDED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&made[i])))) return;
        stale = true;
        d.CachedPSO = {};
        dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&made[i]));
    };
    pool_.Run(cached.size(), make);

    // a cut off record or a stale blob: written again from what was made
    bool rewrite = !known || at != file.size() || stale;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    pipeline_cache_.open(path, std::ios::binary | (rewrite ? std::ios::trunc : std::ios::app));
    if (rewrite) pipeline_cache_.write(reinterpret_cast<const char*>(header), sizeof(header));
    size_t count = 0;
    for (size_t i = 0; i < cached.size(); i++) {
        if (!made[i]) continue;
        pipelines_[Hash64(&cached[i].key, sizeof(PipelineKey))] = made[i];
        if (rewrite) StorePipeline(cached[i].key, cached[i].vs, cached[i].ps, made[i].Get());
        count++;
    }
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    LOGR("D3D12: %zu pipelines made from the cache in %lld ms%s", count, static_cast<long long>(took.count()),
         stale ? ", driver changed" : "");
}

// flushed per pipeline, so an exit or a crash keeps every one before it
void D3D12Backend::StorePipeline(const PipelineKey& k, D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                 ID3D12PipelineState* pso) {
    ComPtr<ID3DBlob> blob;
    if (!pipeline_cache_.is_open() || FAILED(pso->GetCachedBlob(&blob))) return;
    PipelineRecord r{k, uint32_t(vs.BytecodeLength), uint32_t(ps.BytecodeLength), uint32_t(blob->GetBufferSize())};
    pipeline_cache_.write(reinterpret_cast<const char*>(&r), sizeof(r));
    pipeline_cache_.write(static_cast<const char*>(vs.pShaderBytecode), std::streamsize(vs.BytecodeLength));
    pipeline_cache_.write(static_cast<const char*>(ps.pShaderBytecode), std::streamsize(ps.BytecodeLength));
    pipeline_cache_.write(static_cast<const char*>(blob->GetBufferPointer()), std::streamsize(blob->GetBufferSize()));
    pipeline_cache_.flush();
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {view_heap_.Get(), sampler_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    Unbind();
}

void D3D12Backend::Submit(bool wait) {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    BuffersDecayed();
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (!wait) return;
    WaitFence(fence_value_);
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    upload_epoch_++;
    f.views = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

// what is recorded so far goes to the GPU, recording goes on in the same frame
void D3D12Backend::Kick() {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    BuffersDecayed();
    queue_->Signal(fence_.Get(), ++fence_value_);
    CurrentFrame().fence = fence_value_;
    cl_->Reset(CurrentFrame().alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::WaitFence(uint64_t value) {
    if (fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    static bool logged = false;
    if (!std::exchange(logged, true)) LOGR("D3D12: upload ring full, %llu bytes on their own", static_cast<unsigned long long>(size));
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = BufferDesc(size);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Backend::UploadConstants(const void* data, uint32_t size) {
    Alloc a = Upload(size, kCbAlign);
    std::memcpy(a.cpu, data, size);
    return a.gpu;
}

uint32_t D3D12Backend::AllocViews(uint32_t n) {
    Frame& f = CurrentFrame();
    if (f.views + n > kViewsPerFrame) {
        static bool logged = false;
        if (!std::exchange(logged, true)) LOGR("D3D12: descriptor ring full within a frame, waiting for the GPU");
        Submit(true);
    }
    uint32_t i = uint32_t(frame_ % kFrames) * kViewsPerFrame + CurrentFrame().views;
    CurrentFrame().views += n;
    return i;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::ViewCpu(uint32_t i) const {
    return {view_heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * view_inc_};
}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::ViewGuest(uint32_t i) const {
    return {view_heap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * view_inc_};
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::Rtv(uint32_t i) const {
    return {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * rtv_inc_};
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::CopyView(uint32_t i) const {
    return {copy_heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * view_inc_};
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::TextureView(uint32_t i) const {
    return {texture_view_heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * view_inc_};
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::Dsv(uint32_t i) const {
    return {dsv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * dsv_inc_};
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// Targets

// the title never says how tall a target is: the scissor it draws with is its height
uint32_t D3D12Backend::TargetHeight(const State& s) const {
    uint32_t br = s.Reg(kPaScWindowScissorBr);
    return std::max<uint32_t>({Bits(br, 16, 15), s.tiled_height, 1});
}

uint64_t TargetMapKey(const Surface& sf, bool depth, uint32_t width, uint64_t key) {
    uint64_t k = uint64_t(sf.base) | uint64_t(sf.format) << 12 | uint64_t(depth) << 16 | uint64_t(sf.samples) << 17 |
                 uint64_t(width) << 24;
    return k ^ key * 0x9E3779B97F4A7C15ull;
}

// a colour target's view format when the extension left the console's
DXGI_FORMAT ReadFormat(const Target& t) {
    return t.fmt == ColorFormat(t.surface.format) ? ColorReadFormat(t.surface.format) : t.fmt;
}

// the targets_ key of a surface, false when the extension keeps it off the host
bool D3D12Backend::TargetKeyOf(const Surface& sf, bool depth, uint32_t width, uint64_t& map_key) const {
    uint64_t key = 0;
    if (ext_ && !ext_->TargetKey(sf, depth, key)) return false;
    map_key = TargetMapKey(sf, depth, width, key) | (SceneScaled(width) ? kSceneTarget : 0);
    return true;
}

Target* D3D12Backend::EnsureTarget(const Surface& sf, bool depth, uint32_t width, uint32_t height) {
    if (width == 0) return nullptr;
    uint64_t key = 0;
    if (ext_ && !ext_->TargetKey(sf, depth, key)) return nullptr;
    bool scene = SceneScaled(width);
    uint64_t map_key = TargetMapKey(sf, depth, width, key) | (scene ? kSceneTarget : 0);
    Target& t = targets_[map_key];
    if (t.res && t.height >= height) return &t;
    Target old = std::move(t);
    t = {};
    t.depth = depth;
    t.surface = sf;
    t.samples = HostSamples(sf.samples);
    t.key = key;
    t.width = width;
    t.height = std::max(height, old.height);
    // the render scale, less where the target would pass the host's largest texture edge
    float edge = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    t.scale = std::min(scene ? scene_scale_ : scale_, edge / float(std::max(t.width, t.height)));
    t.fmt = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : ColorFormat(sf.format);
    if (ext_) ext_->MakeTarget(sf, depth, t.width, t.height, t.scale, t.fmt);
    t.w = uint32_t(std::ceil(t.width * t.scale));
    t.h = uint32_t(std::ceil(t.height * t.scale));
    DXGI_FORMAT resource = t.fmt == ColorFormat(sf.format) ? ColorResourceFormat(sf.format) : t.fmt;
    D3D12_RESOURCE_DESC d = TextureDesc(depth ? DXGI_FORMAT_R32G8X24_TYPELESS : resource, t.w, t.h);
    d.SampleDesc.Count = t.samples;
    d.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = t.fmt;
    t.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state, &cv, IID_PPV_ARGS(&t.res)))) {
        LOGE("D3D12: target %ux%u format %u failed", t.w, t.h, sf.format);
        if (old.res) (depth ? free_dsv_ : free_rtv_).push_back(old.view);
        targets_.erase(map_key);
        return nullptr;
    }
    std::vector<uint32_t>& pool = depth ? free_dsv_ : free_rtv_;
    if (old.res) {
        t.view = old.view;
        t.tag = old.tag;
        Release(old.res);
        Release(old.fb);
    } else if (!pool.empty()) {
        t.view = pool.back();
        pool.pop_back();
    }
    if (depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = t.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Dsv(t.view));
        cl_->ClearDepthStencilView(Dsv(t.view), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0, 0, 0, nullptr);
    } else {
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = t.fmt;
        rv.ViewDimension = t.samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(t.res.Get(), &rv, Rtv(t.view));
        std::array<float, 4> colour = {};
        if (auto pending = pending_clears_.find(map_key); pending != pending_clears_.end()) {
            colour = pending->second;
            pending_clears_.erase(pending);
        }
        cl_->ClearRenderTargetView(Rtv(t.view), colour.data(), 0, nullptr);
    }
    // a grown target keeps what was drawn
    if (old.res && t.samples == 1 && old.fmt == t.fmt && old.scale == t.scale) {
        Transition(old.res.Get(), old.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{old.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    return &t;
}

// want samples, fewer until the GPU has them for every target format; 0 is 1
uint32_t D3D12Backend::Samples(uint32_t want) {
    static constexpr DXGI_FORMAT kFormats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16_FLOAT,   DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT,   DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    };
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, n};
        HRESULT hr = dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q));
        return SUCCEEDED(hr) && q.NumQualityLevels;
    };
    while (n > 1 && !std::all_of(std::begin(kFormats), std::end(kFormats), has)) n /= 2;
    LOGR("D3D12: MSAA %ux (asked %u)", n, want);
    return n;
}

// every target dropped, their views back in the pools
void D3D12Backend::DropTargets() {
    for (auto& [key, t] : targets_) (t.depth ? free_dsv_ : free_rtv_).push_back(t.view);
    targets_.clear();
    cap_rt_ = nullptr;
    Unbind();
}

// RB_SURFACE_INFO: pitch in pixels, samples 1, 2 or 4
uint32_t SurfacePitch(const State& s) { return Bits(s.Reg(kRbSurfaceInfo), 0, 14); }
uint32_t SurfaceSamples(const State& s) { return 1u << std::min<uint32_t>(Bits(s.Reg(kRbSurfaceInfo), 16, 2), 2); }
uint32_t TargetWidth(const State& s) { return std::max(SurfacePitch(s), s.tiled_width); }

uint32_t ColorInfo(const State& s, uint32_t i) { return s.Reg(i == 0 ? kRbColorInfo : kRbColor1Info + i - 1); }

Surface ColorSurface(const State& s, uint32_t i) {
    uint32_t info = ColorInfo(s, i);
    return {Bits(info, 0, 12), Bits(info, 16, 4), SurfaceSamples(s), SurfacePitch(s)};
}

Target* D3D12Backend::ColorTarget(const State& s, uint32_t i, uint32_t height) {
    return EnsureTarget(ColorSurface(s, i), false, TargetWidth(s), height);
}

Target* D3D12Backend::DepthTarget(const State& s, uint32_t height) {
    uint32_t info = s.Reg(kRbDepthInfo);
    Surface sf{Bits(info, 0, 12), Bits(info, 16, 1), SurfaceSamples(s), SurfacePitch(s)};
    return EnsureTarget(sf, true, TargetWidth(s), height);
}

// EDRAM tiles hold 80 x 16 samples of 32 bits, 40 x 16 of 64
uint32_t D3D12Backend::SpanTiles(const Target& t) const {
    const Surface& sf = t.surface;
    uint32_t tile_w = !t.depth && WideColor(sf.format) ? 40 : 80;
    uint32_t sx = sf.samples == 4 ? 2 : 1;
    uint32_t sy = sf.samples >= 2 ? 2 : 1;
    uint32_t cols = (sf.pitch * sx + tile_w - 1) / tile_w;
    uint32_t rows = (t.height * sy + kTileRows - 1) / kTileRows;
    return cols * rows;
}

void D3D12Backend::ClearTarget(Target& t, uint32_t flags, const float color[4], float z, uint8_t stencil,
                               const D3D12_RECT* rects, uint32_t count) {
    if (t.depth) {
        D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAGS(((flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                ((flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        if (!f) return;
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl_->ClearDepthStencilView(Dsv(t.view), f, z, stencil, count, rects);
    } else if (flags & kClearColors) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl_->ClearRenderTargetView(Rtv(t.view), color, count, rects);
    }
}

// the console clears EDRAM: every target the cleared range holds is cleared with it
void D3D12Backend::ClearSpan(const Target& cleared, uint32_t flags, const float color[4], float z, uint8_t stencil) {
    uint32_t first = cleared.surface.base;
    uint32_t last = std::min(first + SpanTiles(cleared), kEdramTiles);
    for (auto& [key, t] : targets_) {
        uint32_t base = t.surface.base;
        if (&t == &cleared || t.depth != cleared.depth || base < first || base >= last) continue;
        ClearTarget(t, flags, color, z, stencil, nullptr, 0);
    }
}

// Shaders and pipelines

Shader* D3D12Backend::CreateShader(ShaderStage stage, const uint32_t* code, size_t dwords) {
    auto s = std::make_unique<Shader>();
    s->stage = stage;
    s->code.assign(code, code + dwords);
    s->hash = Hash64(code, dwords * 4);
    std::string hlsl = TranslateShader(stage, code, dwords, ShaderOptions{}, s->info);
    if (stage == ShaderStage::Pixel) {
        s->first_fetch = fcip::FindFirstFetch(code, dwords);
        s->material = fcip::ReadsConstant(hlsl, fcip::kMaterialColour);
    }
    s->ambient = fcip::ReadsConstant(hlsl, stage == ShaderStage::Pixel ? fcip::kPsAmbient : fcip::kVsAmbient);
    if (s->info.unknown) LOGR("shader: %u instructions not understood", s->info.unknown);
    if (s->info.memexport) LOGR("shader: memory export left out");
    shaders_.push_back(std::move(s));
    return shaders_.back().get();
}

Variant* D3D12Backend::EnsureVariant(Shader* s, const ShaderOptions& opt) {
    if (!s->last || std::memcmp(&s->last_opt, &opt, sizeof(opt)) != 0) {
        s->last = FindVariant(s, opt);
        s->last_opt = opt;
    }
    if (s->last->building.load(std::memory_order_acquire)) {
        skipped_draws_++;
        return nullptr;
    }
    return s->last->dxil.empty() ? nullptr : s->last;
}

// on a build thread when async
void BuildVariant(Variant& v, const std::string& hlsl, const wchar_t* profile) {
    if (!CompileHlsl(hlsl, profile, v.dxil)) v.dxil.clear();
    v.key = Hash64(v.dxil.data(), v.dxil.size());
    v.building.store(false, std::memory_order_release);
}

Variant* D3D12Backend::FindVariant(Shader* s, const ShaderOptions& opt) {
    uint64_t key = Hash64(&opt, sizeof(opt), s->hash);
    auto& v = s->variants[key];
    if (v) return v.get();
    v = std::make_unique<Variant>();
    std::string hlsl = TranslateShader(s->stage, s->code.data(), s->code.size(), opt, v->info);
    auto traced = [](uint8_t flags) { return (flags & fcip::kShadowRay) != 0; };
    bool rays = std::any_of(std::begin(opt.tex_flags), std::end(opt.tex_flags), traced);
    const wchar_t* profile = L"vs_6_0";
    if (s->stage == ShaderStage::Pixel) profile = rays ? L"ps_6_5" : L"ps_6_0";
    if (rays && helper_lanes_) {
        profile = L"ps_6_6";
        hlsl = "#define HELPER_LANES\n" + hlsl;
    }
    g_perf.shaders++;
    if (FindHlsl(hlsl, profile, v->dxil)) {
        v->key = Hash64(v->dxil.data(), v->dxil.size());
    } else if (Async()) {
        v->building = true;
        posted_shaders_++;
        Post([variant = v.get(), hlsl = std::move(hlsl), profile] { BuildVariant(*variant, hlsl, profile); });
    } else {
        BuildVariant(*v, hlsl, profile);
    }
    return v.get();
}

void FillBlend(const PipelineKey& k, D3D12_BLEND_DESC& d) {
    d.AlphaToCoverageEnable = Bits(k.color_control, 4, 1);
    d.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t b = k.blend[i];
        D3D12_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[i];
        rt.SrcBlend = BlendFactor(Bits(b, 0, 5), false);
        rt.BlendOp = BlendOp(Bits(b, 5, 3));
        rt.DestBlend = BlendFactor(Bits(b, 8, 5), false);
        rt.SrcBlendAlpha = BlendFactor(Bits(b, 16, 5), true);
        rt.BlendOpAlpha = BlendOp(Bits(b, 21, 3));
        rt.DestBlendAlpha = BlendFactor(Bits(b, 24, 5), true);
        // one, zero, add on both is no blending
        rt.BlendEnable = (b & 0x1FFF1FFF) != 0x00010001;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = UINT8(Bits(k.color_mask, 4 * i, 4));
    }
    if (k.motion) d.RenderTarget[kMotionTarget].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (k.depth_only) {
        for (D3D12_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) rt.RenderTargetWriteMask = 0;
        return;
    }
    if (!k.albedo) return;
    D3D12_RENDER_TARGET_BLEND_DESC& albedo = d.RenderTarget[fcip::kAlbedoTarget];
    albedo.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (k.albedo != kAlbedoClear) return;
    // an ocean draw zeroes the albedo the world left under it
    albedo.BlendEnable = TRUE;
    albedo.SrcBlend = D3D12_BLEND_ZERO;
    albedo.DestBlend = D3D12_BLEND_ZERO;
    albedo.BlendOp = D3D12_BLEND_OP_ADD;
    albedo.SrcBlendAlpha = D3D12_BLEND_ZERO;
    albedo.DestBlendAlpha = D3D12_BLEND_ZERO;
    albedo.BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

// RB_DEPTHCONTROL: stencil 0, z 1, z write 2, z func 4, back stencil 7, stencil func, fail, pass, z fail from 8,
// the back face's from 20
void FillDepthStencil(const PipelineKey& k, D3D12_DEPTH_STENCIL_DESC& d) {
    uint32_t c = k.depth_control;
    d.DepthEnable = Bits(c, 1, 1);
    d.DepthWriteMask = Bits(c, 2, 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = Compare(Bits(c, 4, 3));
    d.StencilEnable = Bits(c, 0, 1);
    d.StencilReadMask = UINT8(Bits(k.stencil, 0, 8));
    d.StencilWriteMask = UINT8(Bits(k.stencil, 8, 8));
    auto face = [&](uint32_t at, D3D12_DEPTH_STENCILOP_DESC& f) {
        f.StencilFunc = Compare(Bits(c, at, 3));
        f.StencilFailOp = D3D12_STENCIL_OP(Bits(c, at + 3, 3) + 1);
        f.StencilPassOp = D3D12_STENCIL_OP(Bits(c, at + 6, 3) + 1);
        f.StencilDepthFailOp = D3D12_STENCIL_OP(Bits(c, at + 9, 3) + 1);
    };
    face(8, d.FrontFace);
    face(Bits(c, 7, 1) ? 20 : 8, d.BackFace);
}

// PA_SU_SC_MODE_CNTL: cull front 0, back 1, clockwise front 2, polygon mode 3, front type 5, offsets 11 and 12,
// multisample 15
void FillRasterizer(const PipelineKey& k, D3D12_RASTERIZER_DESC& d) {
    uint32_t m = k.su_mode;
    d.FillMode = Bits(m, 3, 2) && Bits(m, 5, 3) == 1 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.CullMode = Bits(m, 0, 1) ? D3D12_CULL_MODE_FRONT : (Bits(m, 1, 1) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE);
    d.FrontCounterClockwise = !Bits(m, 2, 1);
    // the constant offset rides in the draw's depth offset
    if (Bits(m, 11, 2)) d.SlopeScaledDepthBias = k.poly_scale;
    d.DepthClipEnable = TRUE;
    d.MultisampleEnable = Bits(m, 15, 1);
}

void D3D12Backend::FillPipelineDesc(const PipelineKey& k, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    FillBlend(k, d.BlendState);
    d.SampleMask = UINT_MAX;
    FillRasterizer(k, d.RasterizerState);
    FillDepthStencil(k, d.DepthStencilState);
    d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(k.strip_cut);
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(k.topology);
    for (uint32_t i = 0; i < 4; i++) {
        d.RTVFormats[i] = DXGI_FORMAT(k.rt_formats[i]);
        if (k.rt_formats[i]) d.NumRenderTargets = i + 1;
    }
    if (k.motion) {
        for (uint32_t i = d.NumRenderTargets; i < kMotionTarget; i++) d.RTVFormats[i] = kNullRtvFormat;
        d.RTVFormats[kMotionTarget] = DXGI_FORMAT(k.motion);
        d.NumRenderTargets = kMotionTarget + 1;
    }
    if (k.albedo) {
        for (uint32_t i = d.NumRenderTargets; i < fcip::kAlbedoTarget; i++) d.RTVFormats[i] = kNullRtvFormat;
        d.RTVFormats[fcip::kAlbedoTarget] = kAlbedoFormat;
        d.NumRenderTargets = fcip::kAlbedoTarget + 1;
    }
    d.DSVFormat = DXGI_FORMAT(k.ds_format);
    d.SampleDesc.Count = k.samples;
    static const D3D12_SO_DECLARATION_ENTRY kRayPosition{0, "SV_Position", 0, 0, 3, 0};
    static const UINT kRayStrides[] = {kRayStride};
    if (k.stream_out == kStreamRtao) d.StreamOutput = {&kRayPosition, 1, kRayStrides, 1, 0};
    if (k.stream_out >= kStreamCaster) d.StreamOutput = CasterOutput(k.stream_out);
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PipelineKey& k, const Variant& vs, const Variant* ps) {
    if (last_pso_ && std::memcmp(&k, &last_key_, sizeof(k)) == 0) return last_pso_;
    last_key_ = k;
    last_pso_ = FindPipeline(k, vs, ps);
    return last_pso_;
}

ID3D12PipelineState* D3D12Backend::FindPipeline(const PipelineKey& k, const Variant& vs, const Variant* ps) {
    if (built_ready_.load(std::memory_order_acquire)) TakeBuilt();
    uint64_t key = Hash64(&k, sizeof(k));
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second.Get();
    BuiltPipeline b{k, {vs.dxil.data(), vs.dxil.size()}};
    if (ps) b.ps = {ps->dxil.data(), ps->dxil.size()};
    if (Async()) {
        skipped_draws_++;
        if (!building_.insert(key).second) return nullptr;
        g_perf.pipelines++;
        posted_pipelines_++;
        Post([this, b]() mutable {
            b.pso = MakePipeline(b);
            std::lock_guard lock(built_lock_);
            built_.push_back(std::move(b));
            built_ready_.store(true, std::memory_order_release);
        });
        return nullptr;
    }
    g_perf.pipelines++;
    b.pso = MakePipeline(b);
    if (b.pso) StorePipeline(b.key, b.vs, b.ps, b.pso.Get());
    pipelines_[key] = b.pso;
    return b.pso.Get();
}

// any thread
ComPtr<ID3D12PipelineState> D3D12Backend::MakePipeline(const BuiltPipeline& b) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    FillPipelineDesc(b.key, d);
    d.VS = b.vs;
    d.PS = b.ps;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
        LOGE("D3D12: pipeline %016llx failed", static_cast<unsigned long long>(Hash64(&b.key, sizeof(b.key))));
    }
    return pso;
}

// what the build threads made, kept and written to the cache on the render thread
void D3D12Backend::TakeBuilt() {
    std::lock_guard lock(built_lock_);
    built_ready_ = false;
    for (BuiltPipeline& b : built_) {
        uint64_t key = Hash64(&b.key, sizeof(b.key));
        building_.erase(key);
        if (b.pso) StorePipeline(b.key, b.vs, b.ps, b.pso.Get());
        pipelines_[key] = std::move(b.pso);
    }
    built_.clear();
}

ID3D12PipelineState* D3D12Backend::EnsureResolvePipeline(DXGI_FORMAT rtv, bool ms, bool filter) {
    uint64_t key = uint64_t(rtv) << 2 | uint64_t(filter) << 1 | ms;
    auto& pso = resolve_pipelines_[key];
    if (pso) return pso.Get();
    std::string src = std::string("#define MS ") + (ms ? "1" : "0") + "\n#define FILTER " + (filter ? "1" : "0") + "\n" +
                      kResolveHlsl;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    if (!CompileHlsl("#define vs main\n" + src, L"vs_6_0", vs)) return nullptr;
    if (!CompileHlsl("#define ps main\n" + src, L"ps_6_0", ps)) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs.data(), vs.size()};
    d.PS = {ps.data(), ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Textures

// change detection over large spans: four lanes run side by side instead of one multiply chain
uint64_t SpanHash(const void* data, size_t n, uint64_t seed) {
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    constexpr size_t kLanes = 4;
    auto* p = static_cast<const uint8_t*>(data);
    uint64_t h[kLanes] = {seed, seed + 1, seed + 2, seed + 3};
    size_t i = 0;
    for (; i + kLanes * 8 <= n; i += kLanes * 8) {
        for (size_t k = 0; k < kLanes; k++) {
            uint64_t v;
            std::memcpy(&v, p + i + k * 8, 8);
            h[k] = (h[k] ^ v) * kMul;
            h[k] ^= h[k] >> 29;
        }
    }
    return Hash64(h, sizeof(h), Hash64(p + i, n - i, n));
}

// the guest bytes a texture covers, levels and faces included
uint64_t GuestHash(const xenos::TextureFetch& t, const std::vector<xenos::Level>& levels) {
    const xenos::FormatInfo& fi = xenos::Info(t.format);
    uint64_t h = 0;
    for (const xenos::Level& l : levels) {
        uint64_t bytes = uint64_t(l.pitch_blocks) * l.rows_blocks * fi.bytes;
        uint64_t total = l.face_stride ? uint64_t(l.face_stride) * (t.depth - 1) + bytes : bytes;
        h = SpanHash(Guest(l.address), size_t(std::min<uint64_t>(total, UINT32_MAX - l.address)), h);
    }
    return h;
}

DXGI_FORMAT HostTextureFormat(const xenos::TextureFetch& t) {
    const xenos::FormatInfo& fi = xenos::Info(t.format);
    bool is_signed = std::any_of(std::begin(t.sign), std::end(t.sign), [](xenos::Sign s) {
        return s == xenos::Sign::Signed;
    });
    return is_signed && fi.host_signed != DXGI_FORMAT_UNKNOWN ? fi.host_signed : fi.host;
}

Texture* D3D12Backend::EnsureTexture(const xenos::TextureFetch& t) {
    if (!t.base || xenos::Info(t.format).convert == xenos::Convert::Unsupported) return nullptr;
    // rows shorter than the width: a rescaled shadow map read before its first resolve, backed only at the stock side
    if (t.pitch && t.pitch < t.width) return nullptr;
    std::vector<xenos::Level> levels = xenos::Levels(t);
    if (levels.empty()) return nullptr;
    uint64_t key = Hash64(&levels[0], levels.size() * sizeof(xenos::Level),
                          uint64_t(t.format) | uint64_t(t.dimension) << 8 | uint64_t(t.depth) << 16 |
                              uint64_t(HostTextureFormat(t)) << 32);
    Texture& tex = textures_[key];
    if (!tex.res) {
        bool volume = t.dimension == xenos::Dimension::D3;
        tex.format = HostTextureFormat(t);
        tex.levels = uint32_t(levels.size());
        // block compressed textures are whole blocks on the host
        uint32_t block = xenos::Info(t.format).host_block;
        uint32_t w = (t.width + block - 1) / block * block;
        uint32_t h = (t.height + block - 1) / block * block;
        D3D12_RESOURCE_DESC d = TextureDesc(tex.format, w, h, t.depth, tex.levels, volume);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        tex.state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, tex.state, nullptr,
                                                 IID_PPV_ARGS(&tex.res)))) {
            textures_.erase(key);
            return nullptr;
        }
        tex.checked = ~0ull;
        tex.guest = t;
        tex.guest_levels = levels;
    }
    if (tex.checked != presents_) {
        tex.checked = presents_;
        uint64_t h = tex.hashed_for == presents_ ? NextHash(tex) : GuestHash(t, levels);
        if (h != tex.hash) {
            tex.hash = h;
            UploadTexture(tex, t);
            tex.pack_gen = 0;
        }
    }
    if (tex.pack_gen != texturedumper::Generation()) LatchPackKey(tex, t);
    if (tex.pack_key) LatchPackImage(tex);
    return &tex;
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D12Backend::LatchPackKey(Texture& tex, const xenos::TextureFetch& t) {
    tex.pack_gen = texturedumper::Generation();
    tex.swap = nullptr;
    tex.pack_key = 0;
    if (!texturedumper::Covers(t, tex.format)) return;
    std::vector<xenos::Level> levels = xenos::Levels(t);
    texturedumper::Image img{tex.format, t.width, t.height, std::vector<texturedumper::Level>(1)};
    xenos::LoadLevel(Guest(0), t, levels[0], 0, img.levels[0].px, img.levels[0].pitch);
    uint64_t key = texturedumper::Key(t, img.levels[0]);
    if (texturedumper::WantsDump(key)) {
        img.levels.resize(levels.size());
        for (size_t l = 1; l < levels.size(); l++) {
            xenos::LoadLevel(Guest(0), t, levels[l], 0, img.levels[l].px, img.levels[l].pitch);
        }
        texturedumper::Dump(key, std::move(img));
    }
    // dumping alone keys the bytes too: the pack image is not drawn then
    if (texturedumper::Replacing()) tex.pack_key = key;
}

// the pack image for the texture's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D12Backend::LatchPackImage(Texture& tex) {
    auto it = pack_textures_.find(tex.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tex.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tex.pack_key = 0;
            return;
        }
        it = pack_textures_.try_emplace(tex.pack_key).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tex.swap = &it->second;
    tex.pack_key = 0;
}

void D3D12Backend::CreatePackTexture(Texture& r, const texturedumper::Image& img) {
    r.format = img.format;
    r.levels = uint32_t(img.levels.size());
    D3D12_RESOURCE_DESC d = TextureDesc(r.format, img.width, img.height, 1, r.levels);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    r.state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, r.state, nullptr, IID_PPV_ARGS(&r.res)))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) UploadRows(r.res.Get(), l, img.levels[l].px, img.levels[l].pitch);
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets go
// of its old copy before it draws
void D3D12Backend::ReleasePackTextures() {
    for (auto& [key, pack] : pack_textures_) CurrentFrame().garbage.push_back(pack.res);
    pack_textures_.clear();
    texture_views_.clear();
}

// a texture's hash ahead, unless another thread took it
void TakeHash(Texture& tex) {
    uint32_t waiting = kHashWaiting;
    if (!tex.next_state.compare_exchange_strong(waiting, kHashTaken)) return;
    tex.next_hash = GuestHash(tex.guest, tex.guest_levels);
    tex.next_state.store(kHashDone, std::memory_order_release);
}

// the hash ahead of a texture the render thread wants now: hashed here, or waited for if a pool thread is on it
uint64_t D3D12Backend::NextHash(Texture& tex) {
    TakeHash(tex);
    while (tex.next_state.load(std::memory_order_acquire) != kHashDone) std::this_thread::yield();
    return tex.next_hash;
}

// the textures this frame bound are hashed for the next on the pool threads while the render thread goes on, as
// most are bound again
void D3D12Backend::HashAhead() {
    pool_.Wait();
    hash_ahead_.clear();
    for (auto& [fetch, tex] : frame_textures_) {
        if (!tex || tex->hashed_for == presents_) continue;
        tex->hashed_for = presents_;
        tex->next_state.store(kHashWaiting, std::memory_order_relaxed);
        hash_ahead_.push_back(tex);
    }
    pool_.Start(hash_ahead_.size(), hash_job_);
}

// EnsureTexture once per fetch constant and frame
Texture* D3D12Backend::FrameTexture(const uint32_t fetch[6], const xenos::TextureFetch& t) {
    std::array<uint32_t, 6> key;
    std::copy(fetch, fetch + 6, key.begin());
    auto [it, added] = frame_textures_.try_emplace(key, nullptr);
    if (added) it->second = EnsureTexture(t);
    return it->second;
}

// a level's slice into plane z of an upload footprint
void CopyRows(const std::vector<uint8_t>& rows, uint32_t pitch, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
              UINT row_count, UINT64 row_size, uint32_t z, uint8_t* out) {
    for (UINT r = 0; r < row_count; r++) {
        size_t src = size_t(r) * pitch;
        if (src + row_size > rows.size()) break;
        std::memcpy(out + (size_t(z) * row_count + r) * fp.Footprint.RowPitch, rows.data() + src, size_t(row_size));
    }
}

// each level of each face or array slice is a subresource; a volume's slices are planes of one
void D3D12Backend::UploadTexture(Texture& tex, const xenos::TextureFetch& t) {
    std::vector<xenos::Level> levels = xenos::Levels(t);
    bool volume = t.dimension == xenos::Dimension::D3;
    uint32_t faces = volume ? 1 : t.depth;
    Transition(tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC desc = tex.res->GetDesc();
    std::vector<uint8_t> rows;
    tex.flags = 0;
    g_perf.tex_uploads++;
    for (uint32_t face = 0; face < faces; face++) {
        for (uint32_t l = 0; l < levels.size(); l++) {
            uint32_t sub = l + face * tex.levels;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT row_count;
            UINT64 row_size;
            UINT64 total;
            dev_->GetCopyableFootprints(&desc, sub, 1, 0, &fp, &row_count, &row_size, &total);
            Alloc a = Upload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            g_perf.tex_bytes += total;
            uint32_t planes = volume ? fp.Footprint.Depth : 1;
            const uint8_t* memory = Guest(0);
            for (uint32_t z = 0; z < planes; z++) {
                uint32_t pitch = 0;
                xenos::LoadLevel(memory, t, levels[l], volume ? z : face, rows, pitch);
                if (ext_) ext_->Decoded(t, volume ? z : face, l, rows, pitch, tex.flags);
                CopyRows(rows, pitch, fp, row_count, row_size, z, a.cpu);
            }
            fp.Offset = a.offset;
            D3D12_TEXTURE_COPY_LOCATION dst{tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            dst.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            src.PlacedFootprint = fp;
            cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    }
}

// one 2D subresource from host rows
void D3D12Backend::UploadRows(ID3D12Resource* res, uint32_t sub, const std::vector<uint8_t>& rows, uint32_t pitch) {
    D3D12_RESOURCE_DESC desc = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT row_count;
    UINT64 row_size;
    UINT64 total;
    dev_->GetCopyableFootprints(&desc, sub, 1, 0, &fp, &row_count, &row_size, &total);
    Alloc a = Upload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyRows(rows, pitch, fp, row_count, row_size, 0, a.cpu);
    fp.Offset = a.offset;
    D3D12_TEXTURE_COPY_LOCATION dst{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// the fetch constant's swizzle numbers components as D3D12's component mapping does: 0 to 3, 4 zero, 5 one
UINT ComponentMapping(const xenos::TextureFetch& t) {
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(t.swizzle[0], t.swizzle[1], t.swizzle[2], t.swizzle[3]);
}

// the resolve a fetch reads in place of guest memory, null for none
Resolved* D3D12Backend::FindResolved(const xenos::TextureFetch& t, const TextureBinding& b, ShaderStage stage,
                                     uint32_t slot) {
    uint64_t key = 0;
    if (ext_) ext_->Sample(t.base, stage, slot, key);
    auto resolved = resolved_.find(ResolvedKey(t.base, key));
    if (resolved == resolved_.end()) return nullptr;
    // a block compressed fetch never reads a resolve: the address was reused since
    bool blocks = xenos::Info(t.format).block > 1;
    return b.dimension <= 1 && !blocks ? &resolved->second : nullptr;
}

void ReadFetch(const State& s, uint32_t index, uint32_t out[6]) {
    for (uint32_t k = 0; k < 6; k++) out[k] = Be32(s.fetch + (index * 6 + k) * 4);
}

// what the extension's Sampled makes of each texture the stage binds
void D3D12Backend::TexFlags(const State& s, const ShaderInfo& info, ShaderStage stage, uint8_t flags[32]) {
    for (uint32_t i = 0; i < info.textures.size() && i < 32; i++) {
        uint32_t fetch[6];
        ReadFetch(s, info.textures[i].fetch, fetch);
        xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
        bool resolve = FindResolved(t, info.textures[i], stage, i) != nullptr;
        Texture* tex = resolve ? nullptr : EnsureTexture(t);
        ext_->Sampled(t, resolve, tex ? tex->flags : 0, flags[i]);
    }
}

Resolved* D3D12Backend::SunMap(uint32_t base) {
    auto map = resolved_.find(ResolvedKey(base, 0));
    return map != resolved_.end() && map->second.res ? &map->second : nullptr;
}

// the sharper sun maps and the soft filter, with flags on each pixel shader fetch of a shadow map; false when the draw
// reads no far sun map
bool D3D12Backend::SunCascade(const State& s, const ShaderInfo& info, ShaderOptions& opt) {
    fcip::SunMaps m = fcip::SunMapsDrawn();
    uint32_t filter = fcip::Get().shadow_filter;
    bool soft = filter == fcip::kFilterSoft || filter == fcip::kFilterPcss;
    if ((!m.count && !soft) || info.textures.size() > fcip::kSunFirstSlot) return false;
    // a missing map would read zero depth: it and the coarser ones are left to the far map
    Resolved* maps[fcip::kSunMaps] = {};
    uint32_t count = 0;
    for (; count < m.count; count++) {
        maps[count] = SunMap(m.bases[count]);
        if (!maps[count]) break;
    }
    if (!count && !soft) return false;
    bool any = false;
    for (uint32_t i = 0; i < info.textures.size(); i++) {
        const TextureBinding& b = info.textures[i];
        if (b.fetch != fcip::kSunFetch || b.dimension > 1) continue;
        uint32_t fetch[6];
        ReadFetch(s, b.fetch, fetch);
        uint32_t base = xenos::DecodeTextureFetch(fetch).base;
        bool far_map = m.far_base && base == m.far_base;
        if (far_map && count) opt.tex_flags[i] |= fcip::kSunCascade;
        if (soft) opt.tex_flags[i] |= SoftShadow(base, far_map);
        any = any || (far_map && (count || soft));
    }
    if (!any) return false;
    std::copy(std::begin(maps), std::end(maps), sun_);
    sun_maps_ = m;
    sun_maps_.count = count;
    return true;
}

// original Xbox ocean: one square resolve (the reflection map) beside a wide one (the screen behind the water)
bool D3D12Backend::OceanFlags(const State& s, const ShaderInfo& info, ShaderOptions& opt) {
    if (info.textures.size() > fcip::kOceanOut || !fcip::OceanLookDrawn(ocean_)) return false;
    int reflection = -1;
    bool screen = false;
    for (uint32_t i = 0; i < info.textures.size(); i++) {
        uint32_t fetch[6];
        ReadFetch(s, info.textures[i].fetch, fetch);
        Resolved* r = FindResolved(xenos::DecodeTextureFetch(fetch), info.textures[i], ShaderStage::Pixel, i);
        if (!r) continue;
        if (r->width == r->height && reflection >= 0) return false;
        if (r->width == r->height) reflection = int(i);
        if (r->width > r->height) screen = true;
    }
    if (reflection < 0 || !screen) return false;
    opt.tex_flags[reflection] |= fcip::kOceanReflection;
    opt.tex_flags[fcip::kOceanOut] |= fcip::kOceanReflection;
    return true;
}

// the water's draws into the caustics' target: the wide colour resolve they show behind them takes the light
bool D3D12Backend::CausticsFlags(const State& s, const ShaderInfo& info, ShaderOptions& opt) {
    if (!caustics_lit_ || !caustics_water_ || info.textures.size() > fcip::kCausticsSlot) return false;
    Target* rt = ColorTarget(s, 0, TargetHeight(s));
    if (!rt || MapKey(*rt) != caustics_rt_key_) return false;
    for (uint32_t i = 0; i < info.textures.size(); i++) {
        uint32_t fetch[6];
        ReadFetch(s, info.textures[i].fetch, fetch);
        xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
        Resolved* r = FindResolved(t, info.textures[i], ShaderStage::Pixel, i);
        if (!r || r->width <= r->height || DepthFormat(t.format)) continue;
        opt.tex_flags[fcip::kCausticsOut] = uint8_t(i + 1);
        static bool logged = false;
        if (!std::exchange(logged, true)) LOGR("caustics: a water draw shows the screen at texture %u", i);
        return true;
    }
    static bool missed = false;
    if (!std::exchange(missed, true)) LOGR("caustics: a water draw shows no screen");
    return false;
}

// the soft filter's flags on a fetch of the map at base, none when it is no shadow map
uint8_t D3D12Backend::SoftShadow(uint32_t base, bool far_map) const {
    auto map = shadow_resolves_.find(base);
    if (map == shadow_resolves_.end()) return 0;
    if (far_map && fcip::Get().shadow_filter == fcip::kFilterPcss) return fcip::kSunSoft | fcip::kSunPcss;
    if (far_map) return fcip::kSunSoft;
    return map->second.width < shadow_side_ ? fcip::kShadowSoft | fcip::kShadowHalf : fcip::kShadowSoft;
}

void D3D12Backend::SunMapView(Resolved& r, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = r.format;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(r.res.Get(), &v, at);
}

void D3D12Backend::FillTextureView(const TextureBinding& b, const uint32_t fetch[6], D3D12_CPU_DESCRIPTOR_HANDLE at,
                                   ShaderStage stage, uint32_t slot) {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Shader4ComponentMapping = ComponentMapping(t);
    if (Resolved* found = FindResolved(t, b, stage, slot)) {
        Resolved& r = *found;
        Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        v.Format = r.format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(r.res.Get(), &v, at);
        return;
    }
    Texture* tex = FrameTexture(fetch, t);
    if (tex && tex->swap) tex = tex->swap;
    v.Format = tex ? tex->format : DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (b.dimension) {
        case 2:
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            v.Texture3D.MipLevels = tex ? tex->levels : 1;
            break;
        case 3:
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            v.TextureCube.MipLevels = tex ? tex->levels : 1;
            break;
        default:
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = tex ? tex->levels : 1;
            break;
    }
    // a fetch whose texture the host cannot hold reads zero, as does a dimension the texture lacks
    bool fits = tex && (b.dimension != 3 || t.dimension == xenos::Dimension::Cube) &&
                (b.dimension != 2 || t.dimension == xenos::Dimension::D3);
    if (fits) Transition(tex->res.Get(), tex->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    // made once, copied after
    if (texture_views_.size() >= kTextureViews) {
        static bool logged = false;
        if (!std::exchange(logged, true)) LOGR("D3D12: %u texture views made, starting over", kTextureViews);
        texture_views_.clear();
    }
    ViewKey key{tex, uint32_t(b.dimension), v.Shader4ComponentMapping, fits};
    auto [view, added] = texture_views_.try_emplace(key, uint32_t(texture_views_.size()));
    if (added) dev_->CreateShaderResourceView(fits ? tex->res.Get() : nullptr, &v, TextureView(view->second));
    dev_->CopyDescriptorsSimple(1, at, TextureView(view->second), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// the fetch constant's filters, overridden by the instruction's where it sets them
D3D12_SAMPLER_DESC D3D12Backend::SamplerDesc(const TextureBinding& b, const uint32_t fetch[6]) const {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
    uint32_t mag = b.mag != kFilterFromFetch ? b.mag : t.mag_filter;
    uint32_t min = b.min != kFilterFromFetch ? b.min : t.min_filter;
    uint32_t mip = b.mip != kFilterFromFetch ? b.mip : t.mip_filter;
    uint32_t aniso = b.aniso != kAnisoFromFetch ? b.aniso : t.aniso;
    // the setting replaces the title's: 0 off, else 2 to 16 as a level of 2 to 5
    aniso = std::min<uint32_t>(std::bit_width(std::max(app::Get().aniso, 1u)), 5);
    D3D12_SAMPLER_DESC d{};
    if (aniso > 1) {
        d.Filter = D3D12_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = 1u << (aniso - 1);
    } else {
        d.Filter = D3D12_ENCODE_BASIC_FILTER(min & 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             mag & 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             mip == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    }
    d.AddressU = AddressMode(t.clamp[0]);
    d.AddressV = AddressMode(t.clamp[1]);
    d.AddressW = AddressMode(t.clamp[2]);
    d.MipLODBias = t.lod_bias / 32.0f;
    d.MinLOD = float(t.mip_min);
    // the base map filter samples level mip_min alone
    d.MaxLOD = mip == 2 ? float(t.mip_min) + 0.25f : float(t.mip_max);
    return d;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::EnsureSamplers(const std::vector<D3D12_SAMPLER_DESC>& descs) {
    uint64_t key = Hash64(descs.data(), descs.size() * sizeof(D3D12_SAMPLER_DESC), descs.size());
    auto it = sampler_sets_.find(key);
    if (it == sampler_sets_.end()) {
        uint32_t n = uint32_t(std::max<size_t>(descs.size(), 1));
        if (samplers_used_ + n > kSamplerCount) {
            static bool logged = false;
            if (!std::exchange(logged, true)) LOGR("D3D12: sampler heap full, starting over");
            Submit(true);
            sampler_sets_.clear();
            samplers_used_ = 0;
        }
        it = sampler_sets_.emplace(key, samplers_used_).first;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < descs.size(); i++) {
            dev_->CreateSampler(&descs[i], {cpu.ptr + SIZE_T(samplers_used_ + i) * sampler_inc_});
        }
        samplers_used_ += n;
    }
    return {sampler_heap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(it->second) * sampler_inc_};
}

// copied from the ready made ones, count at most kPsTextures
void D3D12Backend::NullViews(uint32_t first, uint32_t count) {
    if (count) dev_->CopyDescriptorsSimple(count, ViewCpu(first), CopyView(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// a table with texture views from slot first on and their samplers; slots past the shader's hold null views
uint32_t D3D12Backend::BindTextures(const State& s, const ShaderInfo& info, ShaderStage stage, uint32_t first,
                                    uint32_t slots, D3D12_GPU_DESCRIPTOR_HANDLE& samplers) {
    uint32_t used = std::min<uint32_t>(uint32_t(info.textures.size()), slots);
    uint32_t fetch[kPsTextures][6];
    for (uint32_t i = 0; i < used; i++) ReadFetch(s, info.textures[i].fetch, fetch[i]);
    size_t fetch_bytes = used * sizeof(fetch[0]);
    // the shader and fetch constants of the stage's last draw: its tables again. An extension may pick by pass.
    Binds::Stage& kept = binds_.stages[stage == ShaderStage::Vertex ? 0 : 1];
    static Resolved* const kNoSun[fcip::kSunMaps] = {};
    Resolved* const* sun = stage == ShaderStage::Pixel ? sun_ : kNoSun;
    bool same = !ext_ && kept.info == &info && kept.aniso == app::Get().aniso;
    same = same && std::equal(sun, sun + fcip::kSunMaps, kept.sun);
    static const D3D12_GPU_VIRTUAL_ADDRESS kNoRays[fcip::kRayScenes] = {};
    const D3D12_GPU_VIRTUAL_ADDRESS* rays = stage == ShaderStage::Pixel ? ray_scenes_ : kNoRays;
    same = same && std::equal(rays, rays + fcip::kRayScenes, kept.rays);
    ID3D12Resource* fog = stage == ShaderStage::Pixel ? fog_view_ : nullptr;
    same = same && kept.fog == fog;
    ID3D12Resource* caustics = stage == ShaderStage::Pixel ? caustics_view_ : nullptr;
    same = same && kept.caustics == caustics;
    if (same && std::memcmp(kept.fetch, fetch, fetch_bytes) == 0) {
        samplers = kept.samplers;
        return kept.table;
    }
    uint32_t table = AllocViews(first + slots);
    std::vector<D3D12_SAMPLER_DESC>& descs = sampler_descs_;
    descs.clear();
    for (uint32_t i = 0; i < used; i++) {
        const TextureBinding& b = info.textures[i];
        FillTextureView(b, fetch[i], ViewCpu(table + first + i), stage, i);
        descs.push_back(SamplerDesc(b, fetch[i]));
    }
    NullViews(table + first + used, slots - used);
    for (uint32_t i = 0; i < fcip::kSunMaps; i++) {
        if (sun[i]) SunMapView(*sun[i], ViewCpu(table + first + fcip::kSunFirstSlot + i));
    }
    if (std::any_of(rays, rays + fcip::kRayScenes, [](D3D12_GPU_VIRTUAL_ADDRESS a) { return a != 0; })) {
        RayViews(table + first, rays);
    }
    if (fog) ShaderView(fog, kGiFormat, table + first + fcip::kFogSlot);
    if (caustics) ShaderView(caustics, kCausticsFormat, table + first + fcip::kCausticsSlot);
    samplers = EnsureSamplers(descs);
    kept.info = &info;
    std::copy(sun, sun + fcip::kSunMaps, kept.sun);
    std::copy(rays, rays + fcip::kRayScenes, kept.rays);
    kept.fog = fog;
    kept.caustics = caustics;
    kept.aniso = app::Get().aniso;
    std::memcpy(kept.fetch, fetch, fetch_bytes);
    kept.table = table;
    kept.samplers = samplers;
    return table;
}

// Draws

void D3D12Backend::FillKeyStates(const State& s, PipelineKey& k) const {
    for (uint32_t i = 0; i < 4; i++) k.blend[i] = s.Reg(i == 0 ? kRbBlendControl0 : kRbBlendControl1 + i - 1);
    k.color_mask = s.Reg(kRbColorMask);
    k.color_control = s.Reg(kRbColorControl) & 0x10;
    k.depth_control = s.Reg(kRbDepthControl);
    uint32_t sr = s.Reg(kRbStencilRefMask);
    k.stencil = Bits(sr, 8, 8) | Bits(sr, 16, 8) << 8;
    k.su_mode = s.Reg(kPaSuScModeCntl) & 0x9FFF;
    if (Bits(k.su_mode, 11, 2)) k.poly_scale = s.RegF(kPaSuPolyOffsetFrontScale);
}

// the colour targets the pixel shader writes and the mask lets through, and the depth target when tested
bool D3D12Backend::BindTargets(const State& s, const ShaderInfo& ps, PipelineKey& k, Bound& b, bool motion,
                               bool albedo) {
    uint32_t height = TargetHeight(s);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[fcip::kAlbedoTarget + 1];
    uint32_t count = 0;
    b = {TargetWidth(s), height, scale_};
    k.samples = HostSamples(SurfaceSamples(s));
    for (uint32_t i = 0; i < 4; i++) {
        if (!(ps.exports >> i & 1) || !Bits(s.Reg(kRbColorMask), 4 * i, 4)) continue;
        Target* t = ColorTarget(s, i, height);
        if (!t) continue;
        Transition(t->res.Get(), t->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!b.rt) b.rt = t;
        k.rt_formats[i] = t->fmt;
        while (count < i) {
            k.rt_formats[count] = kNullRtvFormat;
            rtv[count++] = Rtv(kNullRtv);
        }
        rtv[count++] = Rtv(t->view);
    }
    Target* ds = nullptr;
    if (Bits(s.Reg(kRbDepthControl), 0, 3)) {
        ds = DepthTarget(s, height);
        if (ds) {
            Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            k.ds_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        }
    }
    if (!count && !ds) return false;
    b.ds = ds;
    b.scale = b.rt ? b.rt->scale : ds->scale;
    if (motion && ds && b.rt && MapKey(*b.rt) == motion_.rt_key && b.rt->samples == 1) {
        while (count < kMotionTarget) {
            k.rt_formats[count] = kNullRtvFormat;
            rtv[count++] = Rtv(kNullRtv);
        }
        Transition(motion_.drawn.Get(), motion_.drawn_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv[count++] = Rtv(kMotionRtv);
        k.motion = kMotionFormat;
        b.motion = true;
    }
    if (albedo && ds && b.rt && MapKey(*b.rt) == gi_.rt_key && b.rt->samples == gi_.samples) {
        // the motion slot's null format is FillPipelineDesc's
        while (count < fcip::kAlbedoTarget) {
            if (count < std::size(k.rt_formats)) k.rt_formats[count] = kNullRtvFormat;
            rtv[count++] = Rtv(kNullRtv);
        }
        Transition(gi_.albedo.Get(), gi_.albedo_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv[count++] = Rtv(kAlbedoRtv);
        k.albedo = gi_.mode;
        b.albedo = true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ds ? Dsv(ds->view) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    TargetViews views;
    for (uint32_t i = 0; i < count; i++) views.rtv[i] = rtv[i].ptr;
    views.dsv = dsv.ptr;
    if (Latch(binds_.targets, views)) cl_->OMSetRenderTargets(count, count ? rtv : nullptr, FALSE, ds ? &dsv : nullptr);
    return true;
}

// the title's viewport over a w x h target: window = ndc * scale + offset, mapped to the host's ndc
void D3D12Backend::FillDrawConstants(const State& s, const Bound& b, DrawConstants& c) const {
    float w = float(b.w);
    float h = float(b.h);
    uint32_t vte = s.Reg(kPaClVteCntl);
    auto vp = [&](uint32_t i, float fallback) { return Bits(vte, i, 1) ? s.RegF(kPaClVportXScale + i) : fallback; };
    float xs = vp(0, 1);
    float xo = vp(1, 0);
    float ys = vp(2, 1);
    float yo = vp(3, 0);
    float zs = vp(4, 1);
    float zo = vp(5, 0);
    if (Bits(s.Reg(kPaSuScModeCntl), 16, 1)) {
        uint32_t wo = s.Reg(kPaScWindowOffset);
        xo += float(SignedBits(wo, 0, 15));
        yo += float(SignedBits(wo, 16, 15));
    }
    // Direct3D 9 pixel centres sit on whole coordinates, the host's half a pixel in
    if (!Bits(s.Reg(kPaSuVtxCntl), 0, 1)) {
        xo += 0.5f;
        yo += 0.5f;
    }
    c = {};
    c.ndc_scale[0] = xs * 2 / w;
    c.ndc_scale[1] = -ys * 2 / h;
    c.ndc_scale[2] = zs;
    c.ndc_offset[0] = xo * 2 / w - 1;
    c.ndc_offset[1] = 1 - yo * 2 / h;
    c.ndc_offset[2] = zo;
    uint32_t cc = s.Reg(kRbColorControl);
    c.alpha_func = Bits(cc, 3, 1) ? Bits(cc, 0, 3) : 7;
    c.alpha_ref = s.RegF(kRbAlphaRef);
    c.vpos_scale = 1 / b.scale;
    c.vtx_fmt = Bits(vte, 8, 3);
    if (SceneScaled(b.w)) {
        c.ndc_offset[0] += 2 * jitter_[0] / (b.w * b.scale);
        c.ndc_offset[1] -= 2 * jitter_[1] / (b.h * b.scale);
    }
    // bits 0 to 5 enable the user planes, 16 turns clipping off
    uint32_t clip = s.Reg(kPaClClipCntl);
    for (uint32_t i = 0; i < 6; i++) {
        bool on = Bits(clip, i, 1) && !Bits(clip, 16, 1);
        if (on) std::memcpy(c.clip[i], s.clip_planes[i], sizeof(c.clip[i]));
    }
}

// the window scissor in the target's pixels, within w x h
Rect D3D12Backend::GuestScissor(const State& s, uint32_t w, uint32_t h) const {
    uint32_t tl = s.Reg(kPaScWindowScissorTl);
    uint32_t br = s.Reg(kPaScWindowScissorBr);
    int32_t x1 = Bits(tl, 0, 15);
    int32_t y1 = Bits(tl, 16, 15);
    int32_t x2 = Bits(br, 0, 15);
    int32_t y2 = Bits(br, 16, 15);
    if (!Bits(tl, 31, 1)) {
        uint32_t wo = s.Reg(kPaScWindowOffset);
        x1 += SignedBits(wo, 0, 15);
        x2 += SignedBits(wo, 0, 15);
        y1 += SignedBits(wo, 16, 15);
        y2 += SignedBits(wo, 16, 15);
    }
    // a draw predicated to some tiles covers only them
    const Rect& p = s.predicated;
    if (p.x2 > p.x1) {
        x1 = std::max(x1, p.x1);
        y1 = std::max(y1, p.y1);
        x2 = std::max(std::min(x2, p.x2), x1);
        y2 = std::max(std::min(y2, p.y2), y1);
    }
    auto clamp = [](int32_t v, uint32_t limit) { return std::clamp(v, 0, int32_t(limit)); };
    return {clamp(x1, w), clamp(y1, h), clamp(x2, w), clamp(y2, h)};
}

D3D12_RECT D3D12Backend::HostRect(const Rect& r, float scale) const {
    return {LONG(r.x1 * scale), LONG(r.y1 * scale), LONG(r.x2 * scale), LONG(r.y2 * scale)};
}

// the topology of a primitive type the host draws as the title gives it; false for the ones made lists
bool DirectTopology(uint32_t prim, D3D12_PRIMITIVE_TOPOLOGY& topo) {
    switch (prim) {
        case kPrimPointList: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; return true;
        case kPrimLineList: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; return true;
        case kPrimLineStrip: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; return true;
        case kPrimTriangleList: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; return true;
        case kPrimTriangleStrip: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; return true;
        default: return false;
    }
}

struct IndexRange {
    uint32_t min;
    uint32_t max;
};

// 16 bit indices widened, a strip cut as the host's 32 bit one; their range without cuts, in one branch free pass
IndexRange CopyIndices16(const uint8_t* in, uint32_t count, bool swap, uint32_t* out) {
    uint16_t lo = 0xFFFF;
    uint16_t hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t v;
        std::memcpy(&v, in + i * 2, 2);
        v = swap ? __builtin_bswap16(v) : v;
        bool cut = v == 0xFFFF;
        lo = std::min(lo, v);
        hi = std::max(hi, uint16_t(cut ? 0 : v));
        out[i] = cut ? 0xFFFFFFFFu : v;
    }
    return {lo, hi};
}

template <class Swap>
IndexRange CopyIndices32(const uint8_t* in, uint32_t count, Swap swap, uint32_t* out) {
    uint32_t lo = UINT32_MAX;
    uint32_t hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v;
        std::memcpy(&v, in + i * 4, 4);
        v = swap(v);
        bool cut = v == 0xFFFF || v == 0xFFFFFFFF;
        lo = std::min(lo, cut ? UINT32_MAX : v);
        hi = std::max(hi, cut ? 0u : v);
        out[i] = v;
    }
    return {lo, hi};
}

IndexRange CopyIndices32(const uint8_t* in, uint32_t count, xenos::Endian e, uint32_t* out) {
    auto swap8in16 = [](uint32_t v) { return (v & 0xFF00FF00u) >> 8 | (v & 0x00FF00FFu) << 8; };
    auto swap8in32 = [](uint32_t v) { return __builtin_bswap32(v); };
    auto swap16in32 = [](uint32_t v) { return v >> 16 | v << 16; };
    auto none = [](uint32_t v) { return v; };
    switch (e) {
        case xenos::Endian::Swap8In16: return CopyIndices32(in, count, swap8in16, out);
        case xenos::Endian::Swap8In32: return CopyIndices32(in, count, swap8in32, out);
        case xenos::Endian::Swap16In32: return CopyIndices32(in, count, swap16in32, out);
        default: return CopyIndices32(in, count, none, out);
    }
}

// the title's indices straight into the upload ring, byte order fixed
bool D3D12Backend::UploadDirectIndices(const DrawArgs& a, uint32_t& min, uint32_t& max) {
    uint32_t format = uint32_t(a.index32) | a.index_endian << 1;
    KeptIndices& kept = kept_indices_[KeptSlot(a.indices, a.count, format)];
    bool same = kept.epoch == upload_epoch_ && kept.data == a.indices && kept.count == a.count && kept.format == format;
    if (a.fenced && same) {
        min = kept.min;
        max = kept.max;
        D3D12_INDEX_BUFFER_VIEW v{kept.gpu, UINT(a.count) * 4, DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&v);
        return true;
    }
    Alloc ib = Upload(size_t(a.count) * 4, 4);
    auto* out = reinterpret_cast<uint32_t*>(ib.cpu);
    xenos::Endian e = xenos::Endian(a.index_endian);
    bool swap16 = e == xenos::Endian::Swap8In16;  // of the swaps only 8 in 16 stays within a 16 bit index
    IndexRange r = a.index32 ? CopyIndices32(a.indices, a.count, e, out)
                             : CopyIndices16(a.indices, a.count, swap16, out);
    if (r.min > r.max) return false;
    min = r.min;
    max = r.max;
    if (a.fenced) kept = {a.indices, a.count, format, upload_epoch_, ib.gpu, min, max};
    D3D12_INDEX_BUFFER_VIEW v{ib.gpu, UINT(a.count) * 4, DXGI_FORMAT_R32_UINT};
    cl_->IASetIndexBuffer(&v);
    return true;
}

// indices into the upload ring as the host draws them: byte order fixed, fans, quads and loops made lists
bool D3D12Backend::UploadIndices(const DrawArgs& a, D3D12_PRIMITIVE_TOPOLOGY& topo, uint32_t& count, uint32_t& min,
                                 uint32_t& max) {
    count = a.count;
    if (a.indices && DirectTopology(a.prim, topo)) return count && UploadDirectIndices(a, min, max);
    std::vector<uint32_t>& src = index_src_;
    src.resize(a.count);
    if (a.indices && a.index32) {
        std::memcpy(src.data(), a.indices, size_t(a.count) * 4);
        xenos::SwapEndian(reinterpret_cast<uint8_t*>(src.data()), size_t(a.count) * 4, xenos::Endian(a.index_endian));
    } else if (a.indices) {
        // of the swaps only 8 in 16 stays within a 16 bit index
        bool swap = xenos::Endian(a.index_endian) == xenos::Endian::Swap8In16;
        for (uint32_t i = 0; i < a.count; i++) {
            uint16_t v;
            std::memcpy(&v, a.indices + i * 2, 2);
            if (swap) v = __builtin_bswap16(v);
            src[i] = v == 0xFFFF ? 0xFFFFFFFF : v;  // strip cut as the host's 32 bit cut value
        }
    } else {
        for (uint32_t i = 0; i < a.count; i++) src[i] = a.start + i;
    }
    std::vector<uint32_t>& out = index_out_;
    out.clear();
    switch (a.prim) {
        case kPrimPointList: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case kPrimLineList: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case kPrimLineStrip: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case kPrimTriangleStrip: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case kPrimLineLoop:
            topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            if (!src.empty()) src.push_back(src[0]);
            break;
        case kPrimTriangleFan:
            topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t i = 1; i + 1 < src.size(); i++) out.insert(out.end(), {src[0], src[i], src[i + 1]});
            break;
        case kPrimQuadList:
            topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t i = 0; i + 3 < src.size(); i += 4) {
                out.insert(out.end(), {src[i], src[i + 1], src[i + 2], src[i], src[i + 2], src[i + 3]});
            }
            break;
        case kPrimTriangleList: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        default:
            static bool logged = false;
            if (!std::exchange(logged, true)) LOGR("D3D12: primitive type %u left out", a.prim);
            return false;
    }
    const std::vector<uint32_t>& list = out.empty() ? src : out;
    count = uint32_t(list.size());
    if (!count) return false;
    min = UINT32_MAX;
    max = 0;
    for (uint32_t v : list) {
        if (v == 0xFFFF || v == 0xFFFFFFFF) continue;  // strip cuts
        min = std::min(min, v);
        max = std::max(max, v);
    }
    if (min > max) return false;
    Alloc ib = Upload(list.size() * 4, 4);
    std::memcpy(ib.cpu, list.data(), list.size() * 4);
    D3D12_INDEX_BUFFER_VIEW v{ib.gpu, UINT(list.size() * 4), DXGI_FORMAT_R32_UINT};
    cl_->IASetIndexBuffer(&v);
    return true;
}

// the vertices the draw reads, copied raw into the upload ring; the vertex shader decodes them
D3D12_GPU_VIRTUAL_ADDRESS D3D12Backend::UploadStreams(const DrawArgs& a, const ShaderInfo& vs, uint32_t min,
                                                      uint32_t max) {
    std::vector<VertexFetchConstant>& fetch = stream_consts_;
    fetch.assign(std::max<size_t>(vs.fetches.size(), 1), {});
    auto& copied = stream_copies_;  // stream and stride to ring offset of vertex 0
    copied.clear();
    Frame& f = CurrentFrame();
    for (size_t k = 0; k < vs.fetches.size(); k++) {
        const StreamFetch& sf = a.streams[k];
        VertexFetchConstant c = sf.fetch;
        c.format |= sf.endian << 8;
        uint32_t stride = std::max(c.stride, 4u);
        auto key = std::make_pair(sf.data, stride);
        auto it = std::find_if(copied.begin(), copied.end(), [&](const auto& c) { return c.first == key; });
        if (it == copied.end()) {
            int64_t low = std::max<int64_t>(int64_t(min) + a.base_vertex, 0);
            int64_t high = std::max<int64_t>(int64_t(max) + a.base_vertex + 1, 0);
            uint64_t first = std::max<uint64_t>(uint64_t(low) * stride, sf.first);
            uint64_t end = std::min<uint64_t>(uint64_t(high) * stride, sf.size);
            uint64_t bytes = end > first ? end - first : 0;
            KeptVertices& kept = kept_vertices_[KeptSlot(sf.data, first, end)];
            bool same = kept.epoch == upload_epoch_ && kept.data == sf.data && kept.first == first && kept.end == end;
            if (!a.fenced || !same) {
                Alloc v = Upload(std::max<uint64_t>(bytes, 16), 16);
                if (v.res != f.upload.Get()) {
                    static bool logged = false;
                    if (!std::exchange(logged, true)) LOGR("D3D12: vertex data past the upload ring reads as zero");
                } else if (a.fenced) {
                    kept = {sf.data, first, end, upload_epoch_, v.offset};
                }
                if (bytes) std::memcpy(v.cpu, sf.data + (first - sf.first), size_t(bytes));
                copied.emplace_back(key, uint32_t(v.offset - first));
            } else {
                copied.emplace_back(key, uint32_t(kept.offset - first));
            }
            it = copied.end() - 1;
        }
        c.address += it->second;
        fetch[k] = c;
    }
    return UploadConstants(fetch.data(), uint32_t(fetch.size() * sizeof(VertexFetchConstant)));
}

// float constants come big endian from the title's banks: swapped as they go up, skipped when unchanged
void D3D12Backend::BindConstants(const State& s, D3D12_GPU_VIRTUAL_ADDRESS fetch, DrawConstants& c, const Target* rt,
                                 uint32_t src_blend) {
    auto bind = [&](uint32_t root, const uint8_t* be, uint32_t bytes) {
        if (!const_gpu_[root] || std::memcmp(const_last_[root], be, bytes) != 0) {
            Alloc a = Upload(bytes, kCbAlign);
            for (uint32_t i = 0; i < bytes; i += 4) {
                uint32_t v = Be32(be + i);
                std::memcpy(a.cpu + i, &v, 4);
            }
            std::memcpy(const_last_[root], be, bytes);
            const_gpu_[root] = a.gpu;
            const_held_[root][0] = const_held_[root][1] = ~0ull;
        }
        if (Latch(binds_.roots[root], const_gpu_[root])) cl_->SetGraphicsRootConstantBufferView(root, const_gpu_[root]);
    };
    if (ext_) {
        float vsc[256][4];
        for (uint32_t i = 0; i < kConstBytes; i += 4) {
            uint32_t v = Be32(s.consts + i);
            std::memcpy(reinterpret_cast<uint8_t*>(vsc) + i, &v, 4);
        }
        ext_->Constants(vsc, c.ext, rt, src_blend);
        // back to the title's byte order, so one upload path serves both
        uint8_t be[kConstBytes];
        for (uint32_t i = 0; i < kConstBytes; i += 4) {
            uint32_t v;
            std::memcpy(&v, reinterpret_cast<uint8_t*>(vsc) + i, 4);
            v = __builtin_bswap32(v);
            std::memcpy(be + i, &v, 4);
        }
        bind(kRootVsConsts, be, kConstBytes);
        bind(kRootPsConsts, s.consts + kConstBytes, kConstBytes);
    } else {
        static const uint64_t kNone[2] = {};
        BindFloats(kRootVsConsts, s.consts, s.vs->info.constants);
        BindFloats(kRootPsConsts, s.consts + kConstBytes, s.ps ? s.ps->info.constants : kNone);
    }
    uint8_t flow[sizeof(FlowConstants)];
    std::memcpy(flow, s.bools, 32);
    std::memcpy(flow + 32, s.loops, 128);
    bind(kRootFlow, flow, sizeof(flow));
    cl_->SetGraphicsRootConstantBufferView(kRootDraw, UploadConstants(&c, sizeof(c)));
    cl_->SetGraphicsRootConstantBufferView(kRootFetch, fetch);
}

// a stage's float constants: only the blocks its shader reads are compared and swapped into the ring; an upload is
// kept while it holds every block a draw reads
void D3D12Backend::BindFloats(uint32_t root, const uint8_t* be, const uint64_t (&read)[2]) {
    constexpr uint32_t kBlock = 32;  // two constants, a bit of read
    uint64_t(&held)[2] = const_held_[root];
    bool same = const_gpu_[root] && !(read[0] & ~held[0]) && !(read[1] & ~held[1]);
    for (uint32_t w = 0; w < 2 && same; w++) {
        for (uint64_t m = read[w]; m && same; m &= m - 1) {
            uint32_t at = (w * 64 + uint32_t(std::countr_zero(m))) * kBlock;
            same = std::memcmp(const_last_[root] + at, be + at, kBlock) == 0;
        }
    }
    if (!same) {
        Alloc a = Upload(kConstBytes, kCbAlign);
        for (uint32_t w = 0; w < 2; w++) {
            for (uint64_t m = read[w]; m; m &= m - 1) {
                uint32_t at = (w * 64 + uint32_t(std::countr_zero(m))) * kBlock;
                for (uint32_t i = 0; i < kBlock; i += 4) {
                    uint32_t v = Be32(be + at + i);
                    std::memcpy(a.cpu + at + i, &v, 4);
                }
                std::memcpy(const_last_[root] + at, be + at, kBlock);
            }
        }
        held[0] = read[0];
        held[1] = read[1];
        const_gpu_[root] = a.gpu;
    }
    if (Latch(binds_.roots[root], const_gpu_[root])) cl_->SetGraphicsRootConstantBufferView(root, const_gpu_[root]);
}

// the texture tables of both stages and the vertex bytes; first, as running out of descriptors submits the list
void D3D12Backend::BindViews(const State& s, const ShaderInfo& vs, const ShaderInfo& ps) {
    D3D12_GPU_DESCRIPTOR_HANDLE vs_samplers;
    D3D12_GPU_DESCRIPTOR_HANDLE ps_samplers;
    uint32_t vs_views = BindTextures(s, vs, ShaderStage::Vertex, 1, kVsTextures, vs_samplers);
    uint32_t ps_views = BindTextures(s, ps, ShaderStage::Pixel, 0, kPsTextures, ps_samplers);
    uint32_t raw = kPsTextures + uint32_t(frame_ % kFrames);
    dev_->CopyDescriptorsSimple(1, ViewCpu(vs_views), CopyView(raw), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (Latch(binds_.roots[kRootVsViews], ViewGuest(vs_views).ptr)) {
        cl_->SetGraphicsRootDescriptorTable(kRootVsViews, ViewGuest(vs_views));
    }
    if (Latch(binds_.roots[kRootPsViews], ViewGuest(ps_views).ptr)) {
        cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(ps_views));
    }
    if (Latch(binds_.roots[kRootVsSamplers], vs_samplers.ptr)) {
        cl_->SetGraphicsRootDescriptorTable(kRootVsSamplers, vs_samplers);
    }
    if (Latch(binds_.roots[kRootPsSamplers], ps_samplers.ptr)) {
        cl_->SetGraphicsRootDescriptorTable(kRootPsSamplers, ps_samplers);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE heap = view_heap_->GetGPUDescriptorHandleForHeapStart();
    if (dev5_ && Latch(binds_.roots[kRootHeap], heap.ptr)) cl_->SetGraphicsRootDescriptorTable(kRootHeap, heap);
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(D3D12_PRIMITIVE_TOPOLOGY topo) {
    if (topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST) return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    if (topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

void D3D12Backend::Draw(const State& s, const DrawArgs& a) {
    if (!s.vs || !s.consts || !s.fetch || !a.count) return;
    // a cull of both faces draws nothing
    if (Bits(s.Reg(kPaSuScModeCntl), 0, 2) == 3) return;
    if (cap_) CapStep(ColorTarget(s, 0, TargetHeight(s)));
    // EndDraw on every way out
    struct DrawScope {
        Extension* ext;
        ~DrawScope() {
            if (ext) ext->EndDraw();
        }
    } scope{ext_};
    if (ext_) ext_->BeginDraw(s);
    ShaderOptions vs_opt;
    std::copy(std::begin(s.link.export_map), std::end(s.link.export_map), vs_opt.export_map);
    ShaderOptions ps_opt;
    ps_opt.vpos_reg = s.link.vpos_reg;
    ps_opt.alpha_test = Bits(s.Reg(kRbColorControl), 3, 1) && Bits(s.Reg(kRbColorControl), 0, 3) != 7;
    if (ext_) {
        TexFlags(s, s.vs->info, ShaderStage::Vertex, vs_opt.tex_flags);
        if (s.ps) TexFlags(s, s.ps->info, ShaderStage::Pixel, ps_opt.tex_flags);
    }
    std::fill(std::begin(sun_), std::end(sun_), nullptr);
    bool sun = s.ps && SunCascade(s, s.ps->info, ps_opt);
    std::fill(std::begin(ray_scenes_), std::end(ray_scenes_), 0);
    if (s.ps) RayShadows(s, s.ps->info, ps_opt);
    bool ocean = s.ps && !sun && !ray_scenes_[0] && OceanFlags(s, s.ps->info, ps_opt);
    fog_view_ = FogFlags(s, ps_opt) ? fog_[0].Get() : nullptr;
    bool caustics = s.ps && !sun && !ray_scenes_[0] && CausticsFlags(s, s.ps->info, ps_opt);
    caustics_view_ = caustics ? caustics_.Get() : nullptr;
    Variant* vs = EnsureVariant(s.vs, vs_opt);
    // no pixel shader: a depth only draw
    Variant* ps = s.ps ? EnsureVariant(s.ps, ps_opt) : nullptr;
    if (!vs || (s.ps && !ps) || (!vs->info.fetches.empty() && !a.streams)) return;
    static const ShaderInfo kNoPixelShader;
    const ShaderInfo& ps_info = ps ? ps->info : kNoPixelShader;
    BindViews(s, vs->info, ps_info);

    PipelineKey k;
    k.vs = vs->key;
    k.ps = ps ? ps->key : 0;
    Bound bound;
    PipelineKey unbound = k;
    bool motion = MotionWanted(s, ps_info);
    if (cap_ && motion_.active && !motion) std::fprintf(cap_, "     motion: none, blended or no depth write\n");
    bool albedo = AlbedoWanted(s, ps_info);
    if (!BindTargets(s, ps_info, k, bound, motion, albedo)) return;
    if (ext_ && ext_->Bound(*this, s, bound.rt, bound.ds)) {
        k = unbound;
        if (!BindTargets(s, ps_info, k, bound, motion, albedo)) return;
    }
    if (bound.motion) {
        vs_opt.motion = true;
        ps_opt.motion = true;
    }
    if (bound.albedo) AlbedoFlags(s, ps_info, ps_opt);
    if (bound.motion || bound.albedo) {
        vs = EnsureVariant(s.vs, vs_opt);
        ps = EnsureVariant(s.ps, ps_opt);
        if (!vs || !ps) return;
        k.vs = vs->key;
        k.ps = ps->key;
    }
    FillKeyStates(s, k);
    if (!k.ds_format) k.depth_control = 0;
    D3D12_PRIMITIVE_TOPOLOGY topo;
    uint32_t count = 0;
    uint32_t min = 0;
    uint32_t max = 0;
    if (a.fences != draw_fences_) {
        draw_fences_ = a.fences;
        upload_epoch_++;
    }
    if (!UploadIndices(a, topo, count, min, max)) return;
    k.topology = TopologyType(topo);
    bool strip = topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topo == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    // PA_SU_SC_MODE_CNTL multi_prim_ib_ena: strips restart at an all ones index
    if (strip && Bits(s.Reg(kPaSuScModeCntl), 21, 1)) k.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    CasterDraw caster;
    uint32_t caster_vertices = CasterVertices(s, a, k, topo, count, bound, ps_opt.alpha_test, caster);
    uint32_t ray_vertices = caster_vertices ? 0 : RayVertices(a, k, topo, count, ps_opt.alpha_test);
    k.stream_out = ray_vertices ? kStreamRtao : caster.stream;
    ID3D12PipelineState* depth_pass = s.ps ? EnsureDepthPass(s, ps_opt, *vs, k) : nullptr;
    ID3D12PipelineState* pso = EnsurePipeline(k, *vs, ps);
    if (!pso) return;
    g_perf.draws++;

    DrawConstants c;
    FillDrawConstants(s, bound, c);
    // the console's offset is in depth units, D3D12's constant bias on float depth scales with the depth
    if (Bits(k.su_mode, 11, 2)) c.ndc_offset[2] += s.RegF(kPaSuPolyOffsetFrontScale + 1);
    if (sun) {
        c.ext[0] = float(sun_maps_.distance);
        c.ext[1] = float(sun_maps_.count);
        // both guest sides are multiples of 8 up to 8192: the far one's eighths whole, the near one's over 2048 below
        c.ext[2] = float(sun_maps_.side / 8) + float(sun_maps_.near_side / 8) / 2048;
        c.ext[3] = sun_maps_.depth_ratio;
    }
    if (ray_scenes_[0]) std::copy(std::begin(ray_spread_), std::end(ray_spread_), c.ext);
    if (ocean) {
        c.ext[0] = ocean_.amount;
        c.ext[1] = ocean_.pivot;
    }
    // the source factor of colour 0, 1 without blending
    uint32_t b0 = k.blend[0];
    uint32_t src_blend = (b0 & 0x1FFF1FFF) != 0x00010001 ? Bits(b0, 0, 5) : 1;
    BindConstants(s, UploadStreams(a, vs->info, min, max), c, bound.rt, src_blend);
    if (bound.motion) BindMotion(s, a, vs->info);
    D3D12_VIEWPORT vp{0, 0, std::ceil(bound.w * bound.scale), std::ceil(bound.h * bound.scale), 0, 1};
    D3D12_RECT sc = HostRect(GuestScissor(s, bound.w, bound.h), bound.scale);
    if (Latch(binds_.viewport, vp)) cl_->RSSetViewports(1, &vp);
    if (Latch(binds_.scissor, sc)) cl_->RSSetScissorRects(1, &sc);
    float blend[4];
    for (uint32_t i = 0; i < 4; i++) blend[i] = s.RegF(kRbBlendRed + i);
    if (Latch(binds_.blend, blend)) cl_->OMSetBlendFactor(blend);
    uint32_t stencil_ref = Bits(s.Reg(kRbStencilRefMask), 0, 8);
    if (Latch(binds_.stencil_ref, stencil_ref)) cl_->OMSetStencilRef(stencil_ref);
    if (Latch(binds_.pso, pso)) cl_->SetPipelineState(pso);
    if (Latch(binds_.topology, topo)) cl_->IASetPrimitiveTopology(topo);
    if (bound.ds && ShadowWidth(bound.ds->width)) {
        KeepShadowTarget(*bound.ds);
        if (bound.rt) KeepShadowTarget(*bound.rt);
        shadow_drawn_ = true;
        Stamp(kStampShadowMap);  // diag
    } else {
        uint8_t kind = kStampOther;  // diag
        if (ray_scenes_[0]) kind = ps_opt.alpha_test ? kStampTracedAlpha : kStampTraced;  // diag
        Stamp(kind);  // diag
        if (ray_scenes_[0]) stamp_draws_[ps_opt.alpha_test ? 1 : 0]++;  // diag
    }
    if (depth_pass) {
        cl_->SetPipelineState(depth_pass);
        cl_->DrawIndexedInstanced(count, 1, 0, a.base_vertex, 0);
        cl_->SetPipelineState(pso);
    }
    if (ray_vertices) StreamOut(c, ray_vertices);
    if (caster_vertices) StreamOutCasters(caster, c);
    cl_->DrawIndexedInstanced(count, 1, 0, a.base_vertex, 0);
    if (ray_vertices || caster_vertices) cl_->SOSetTargets(0, 1, nullptr);
    binds_.valid = true;
    if (cap_) {
        uint32_t rt = bound.rt ? bound.rt->surface.base : 0;
        uint32_t ds = bound.ds ? bound.ds->surface.base : 0;
        std::fprintf(cap_, "%04u draw prim %u count %u into %ux%u rt %u ds %u vs %016llx ps %016llx\n", cap_i_++,
                     a.prim, count, bound.w, bound.h, rt, ds, static_cast<unsigned long long>(k.vs),
                     static_cast<unsigned long long>(k.ps));
        const Rect& p = s.predicated;
        std::fprintf(cap_, "     scissor %ld,%ld,%ld,%ld tiles %d,%d,%d,%d\n", sc.left, sc.top, sc.right, sc.bottom, p.x1,
                     p.y1, p.x2, p.y2);
        if (gi_.mode) CapIllumination(s, k, bound, caster);
        CapTextures(s, ps_info);
    }
}

// F3: what a camera pass draw gave global illumination
void D3D12Backend::CapIllumination(const State& s, const PipelineKey& k, const Bound& b, const CasterDraw& caster) {
    const char* albedo = b.albedo ? (k.albedo == kAlbedoClear ? "cleared" : "written") : "none";
    if (caster.view) {
        const char* kind = caster.alpha ? "alpha tested" : "opaque";
        std::fprintf(cap_, "     illumination: albedo %s, scene %s, %u records from %u\n", albedo, kind, caster.count,
                     caster.info[0]);
        return;
    }
    int first = s.ps ? s.ps->first_fetch.reg : -1;
    std::fprintf(cap_, "     illumination: albedo %s, scene none (capture %u, z write %u, blend %08x, coverage %u, "
                 "first fetch %d)\n", albedo, gi_.capture ? 1u : 0u, Bits(k.depth_control, 2, 1), k.blend[0],
                 Bits(k.color_control, 4, 1), first);
}

// D3D Clear, within the scissor: a whole target clear also clears the targets its EDRAM range holds
void D3D12Backend::Clear(const State& s, uint32_t flags, const float color[4], float z, uint32_t stencil,
                         const Rect* rect) {
    Stamp(kStampOther);  // diag
    if (flags & kClearOcclusion) {
        if (rect) Occlusion(s, UnpackOcclusion(color, z, stencil, *rect));
        return;
    }
    if (flags & kClearOcclusionScene) {
        BeginRayScene();
        return;
    }
    if (flags & kClearIlluminationScene) {
        BeginIllumination(s);
        return;
    }
    if (flags & kClearIlluminationWorld) {
        std::copy(color, color + 3, gi_.ambient);
        gi_.capture = false;
        return;
    }
    if (flags & (kClearIlluminationOcean | kClearIlluminationPastOcean)) {
        if (gi_.mode) gi_.mode = (flags & kClearIlluminationOcean) ? kAlbedoClear : kAlbedoWrite;
        return;
    }
    if (flags & kClearIllumination) {
        if (rect) Illumination(s, UnpackOcclusion(color, z, stencil, *rect));
        return;
    }
    if (flags & kClearFog) {
        if (rect) VolumetricFog(s, UnpackOcclusion(color, z, stencil, *rect));
        return;
    }
    if (flags & kClearFogEnd) {
        fog_near_ = false;
        return;
    }
    if (flags & kClearClouds) {
        if (rect) VolumetricClouds(s, UnpackOcclusion(color, z, stencil, *rect));
        return;
    }
    if (flags & kClearCaustics) {
        if (rect) Caustics(s, UnpackOcclusion(color, z, stencil, *rect));
        return;
    }
    if (flags & (kClearCausticsWater | kClearCausticsDry)) {
        caustics_water_ = (flags & kClearCausticsWater) != 0;
        return;
    }
    if (flags & (kClearFogMap | kClearFogMapEnd)) {
        fog_map_ = (flags & kClearFogMap) != 0;
        return;
    }
    if (flags & kClearMotionBegin) {
        BeginMotion(s);
        return;
    }
    if (flags & (kClearMotionPause | kClearMotionResume)) {
        motion_.active = motion_.open && (flags & kClearMotionResume);
        if (cap_) std::fprintf(cap_, "motion %s\n", motion_.active ? "resumed" : "paused");
        return;
    }
    if (flags & kClearSceneBegin) {
        BeginScene();
        return;
    }
    if (flags & kClearSceneEnd) {
        EndMotion(s);
        EndScene();
        return;
    }
    if (flags & kClearHudBegin) {
        KeepHudless(s);
        return;
    }
    g_perf.clears++;
    uint32_t height = TargetHeight(s);
    uint32_t width = TargetWidth(s);
    Rect r = GuestScissor(s, width, height);
    if (rect) {
        r = {std::max(r.x1, rect->x1), std::max(r.y1, rect->y1), std::min(r.x2, rect->x2), std::min(r.y2, rect->y2)};
    }
    if (r.x1 >= r.x2 || r.y1 >= r.y2) return;
    bool whole = r.x1 == 0 && r.y1 == 0 && r.x2 == int32_t(width) && r.y2 == int32_t(height);
    auto clear = [&](Target* t, uint32_t which) {
        if (!t) return;
        D3D12_RECT host = HostRect(r, t->scale);
        ClearTarget(*t, which, color, z, uint8_t(stencil), whole ? nullptr : &host, whole ? 0 : 1);
        if (whole) ClearSpan(*t, which, color, z, uint8_t(stencil));
    };
    for (uint32_t i = 0; i < 4; i++) {
        if (!(flags >> i & 1)) continue;
        Surface sf = ColorSurface(s, i);
        uint64_t key = 0;
        if (!TargetKeyOf(sf, false, width, key)) continue;
        if (targets_.count(key)) {
            clear(ColorTarget(s, i, height), 1u << i);
            continue;
        }
        // nothing drew it yet: made in the clear colour at its first use
        std::copy(color, color + 4, pending_clears_[key].begin());
        Target span;
        span.surface = sf;
        span.width = width;
        span.height = height;
        if (whole) ClearSpan(span, 1u << i, color, z, uint8_t(stencil));
    }
    if (flags & (kClearZ | kClearStencil)) {
        Target* ds = DepthTarget(s, height);
        clear(ds, flags & (kClearZ | kClearStencil));
        if (flags & kClearZ) DropCasters(ds);
    }
}

// Ambient occlusion

// multiplies occlusion from the bound depth into the bound colour target
void D3D12Backend::Occlusion(const State& s, const OcclusionArgs& a) {
    bool traced = std::exchange(ray_capture_, false);
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples) return;
    if (!EnsureOcclusionTargets(rt->w, rt->h)) return;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    D3D12_RECT rect = HostRect(GuestScissor(s, b.w, b.h), b.scale);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    float c[kAoConstants];
    OcclusionConstants(a, d, rect, rt->w, rt->h, c);
    // RTAO without inline ray tracing is GTAO
    D3D12_GPU_VIRTUAL_ADDRESS scene = traced ? BuildRayScene(a) : 0;
    const char* entry = a.method == kAoSsao ? "occlusion" : "gtao";
    if (scene) entry = "rtao";
    AoPass raw{.entry = entry, .rtv = Rtv(kAoRtv), .scene = scene};
    AoPass across{.entry = "occlusion_blur", .rtv = Rtv(kAoRtv + 1), .src = ao_[0].Get(), .step_x = 1};
    AoPass down{.entry = "occlusion_apply", .out = rt->fmt, .samples = rt->samples, .blend = true, .rtv = Rtv(rt->view),
                .src = ao_[1].Get(), .step_y = 1};
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool ok = OcclusionPass(raw, *ds, c, rect);
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ok = ok && OcclusionPass(across, *ds, c, rect);
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ok) OcclusionPass(down, *ds, c, rect);
    Unbind();
}

bool D3D12Backend::EnsureOcclusionTargets(uint32_t w, uint32_t h) {
    if (ao_[kAoTargets - 1] && ao_w_ == w && ao_h_ == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kAoFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ao_w_ = ao_h_ = 0;
    for (int i = 0; i < kAoTargets; i++) {
        if (ao_[i]) Release(std::move(ao_[i]));
        ao_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, ao_state_[i], nullptr,
                                                   IID_PPV_ARGS(&ao_[i]));
        if (FAILED(hr)) {
            LOGE("D3D12: no %ux%u ambient occlusion target", w, h);
            return false;
        }
        dev_->CreateRenderTargetView(ao_[i].Get(), nullptr, Rtv(kAoRtv + i));
    }
    ao_w_ = w;
    ao_h_ = h;
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureOcclusionPipeline(const AoPass& p, bool ms_depth) {
    bool rt = p.scene != 0;
    uint64_t seed = uint64_t(p.out) | uint64_t(p.samples) << 8 | uint64_t(p.blend) << 16 | uint64_t(ms_depth) << 17;
    seed |= uint64_t(p.rgb) << 18 | uint64_t(p.lighten) << 19;
    auto& pso = ao_pipelines_[Hash64(p.entry, std::strlen(p.entry), seed)];
    if (pso) return pso.Get();
    std::string src = std::string(ms_depth ? "#define MS 1\n" : "") + (rt ? "#define RT 1\n" : "") + kViewHlsl;
    if (p.gi) src += std::string(kGiViewsHlsl) + fcip::kCasterHitHlsl + kGiHlsl;
    else if (p.fog) src += kFogHlsl;
    else if (p.clouds) src += kCloudHlsl;
    else if (p.caustics) src += std::string(fcip::kCausticsFadeHlsl) + kCausticsHlsl;
    else src += kAoHlsl;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    std::string entry = std::string("#define ") + p.entry + " main\n";
    if (!CompileHlsl("#define vs main\n" + src, L"vs_6_0", vs)) return nullptr;
    if (!CompileHlsl(entry + src, rt || p.gi ? L"ps_6_5" : L"ps_6_0", ps)) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs.data(), vs.size()};
    d.PS = {ps.data(), ps.size()};
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (p.blend) OcclusionBlend(b);
    if (p.lighten) CausticsBlend(b);
    // a UNORM target clamps a blend's source to 0, so light taken away is written whole
    if (p.rgb) b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                         D3D12_COLOR_WRITE_ENABLE_BLUE;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = p.out;
    d.SampleDesc.Count = p.samples;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// one triangle over the whole target, drawn within rect
bool D3D12Backend::OcclusionPass(const AoPass& p, Target& depth, float c[kAoConstants], const D3D12_RECT& rect) {
    ID3D12PipelineState* pso = EnsureOcclusionPipeline(p, depth.samples > 1);
    if (!pso) return false;
    c[18] = p.step_x;
    c[19] = p.step_y;
    uint32_t views = AllocViews(kPsTextures);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    v.ViewDimension = depth.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(depth.res.Get(), &v, ViewCpu(views));
    ShaderView(p.src, p.src_format, views + 1);
    uint32_t used = 2;
    if (p.scene) {
        D3D12_SHADER_RESOURCE_VIEW_DESC r{};
        r.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        r.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        r.RaytracingAccelerationStructure.Location = p.scene;
        dev_->CreateShaderResourceView(nullptr, &r, ViewCpu(views + used++));
    }
    if (p.gi) {
        // kGiViewsHlsl's: the albedo, the scene's colour, the casters and their records
        if (!p.scene) NullViews(views + used++, 1);
        TargetView(gi_.albedo.Get(), kAlbedoFormat, gi_.samples, views + used++);
        TargetView(p.colour, p.colour_format, 1, views + used++);
        CasterViews(views + used);
        used += 2;
        if (p.sun) SunMapView(*p.sun, ViewCpu(views + used));
        else NullViews(views + used, 1);
        used++;
        if (fog_near_) ShaderView(fog_[0].Get(), kGiFormat, views + used);
        else NullViews(views + used, 1);
        used++;
        D3D12_GPU_DESCRIPTOR_HANDLE heap = view_heap_->GetGPUDescriptorHandleForHeapStart();
        cl_->SetGraphicsRootDescriptorTable(kRootHeap, heap);
    }
    if (p.fog || p.caustics) {
        if (p.sun) SunMapView(*p.sun, ViewCpu(views + used));
        else NullViews(views + used, 1);
        used++;
        if (p.shade) ShaderView(p.shade, DXGI_FORMAT_R32_FLOAT, views + used);
        else NullViews(views + used, 1);
        used++;
    }
    NullViews(views + used, kPsTextures - used);
    cl_->OMSetRenderTargets(1, &p.rtv, FALSE, nullptr);
    uint32_t shift = p.half ? 1 : 0;
    D3D12_VIEWPORT vp{0, 0, float((depth.w + shift) >> shift), float((depth.h + shift) >> shift), 0, 1};
    D3D12_RECT scissor{rect.left >> shift, rect.top >> shift, (rect.right + LONG(shift)) >> shift,
                       (rect.bottom + LONG(shift)) >> shift};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootFlow, UploadConstants(c, kAoConstants * sizeof(float)));
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
    return true;
}

// Volumetric fog

// fog marched through its own sun map, else the far one, blended over the bound colour target
void D3D12Backend::VolumetricFog(const State& s, const OcclusionArgs& a) {
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples) return;
    if (!EnsureFogTargets(rt->w, rt->h)) return;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    D3D12_RECT rect = HostRect(GuestScissor(s, b.w, b.h), b.scale);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    FogView view = FogViewAt(a.method);
    fcip::SunMaps maps = fcip::SunMapsDrawn();
    uint32_t map = maps.fog_base ? maps.fog_base : maps.far_base;
    Resolved* sun = view.shadowed ? SunMap(map) : nullptr;
    ID3D12Resource* shade = view.shaded ? ShadeTexture() : nullptr;
    float c[kAoConstants];
    OcclusionConstants(a, d, rect, rt->w, rt->h, c);
    c[16] = sun ? 1.0f : 0.0f;
    c[17] = shade ? 1.0f : 0.0f;
    std::copy(std::begin(view.sun), std::end(view.sun), c + 32);
    std::copy(std::begin(view.height), std::end(view.height), c + 48);
    std::copy(std::begin(view.sun_colour), std::end(view.sun_colour), c + 52);
    std::copy(std::begin(view.terrain), std::end(view.terrain), c + 56);
    AoPass march{.entry = "fog_march", .out = kGiFormat, .rtv = Rtv(kFogRtv), .fog = true, .sun = sun,
                 .shade = shade};
    AoPass across{.entry = "fog_blur", .out = kGiFormat, .rtv = Rtv(kFogRtv + 1), .src = fog_[0].Get(), .step_x = 1,
                  .src_format = kGiFormat, .fog = true};
    AoPass down{.entry = "fog_apply", .out = rt->fmt, .samples = rt->samples, .blend = true, .rtv = Rtv(rt->view),
                .src = fog_[1].Get(), .step_y = 1, .src_format = kGiFormat, .fog = true};
    AoPass medium{.entry = "fog_medium", .out = kGiFormat, .rtv = Rtv(kFogRtv), .src = fog_[1].Get(), .step_y = 1,
                  .src_format = kGiFormat, .fog = true};
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(fog_[0].Get(), fog_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool ok = OcclusionPass(march, *ds, c, rect);
    Transition(fog_[0].Get(), fog_state_[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(fog_[1].Get(), fog_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ok = ok && OcclusionPass(across, *ds, c, rect);
    Transition(fog_[1].Get(), fog_state_[1], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ok = ok && OcclusionPass(down, *ds, c, rect);
    Transition(fog_[0].Get(), fog_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ok = ok && OcclusionPass(medium, *ds, c, rect);
    Transition(fog_[0].Get(), fog_state_[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    fog_near_ = ok;
    fog_rt_key_ = MapKey(*rt);
    Unbind();
}

// a draw into the fog's target past its pass takes the fog in its pixel shader, its views short of fcip::kFogSlot
bool D3D12Backend::FogFlags(const State& s, ShaderOptions& opt) {
    if (!fog_near_ || !s.ps || !(s.ps->info.exports & 1) || s.ps->info.textures.size() > fcip::kFogSlot) return false;
    Target* rt = ColorTarget(s, 0, TargetHeight(s));
    if (!rt || MapKey(*rt) != fog_rt_key_) return false;
    opt.tex_flags[fcip::kFogOut] = FogMode(s.Reg(kRbBlendControl0));
    return true;
}

// the terrain shade, uploaded when a new one was published; null before the first
ID3D12Resource* D3D12Backend::ShadeTexture() {
    gpu::TerrainShade shade;
    uint32_t generation = gpu::TerrainShadeSince(shade_generation_, shade);
    if (generation == shade_generation_) return shade_.Get();
    shade_generation_ = generation;
    if (shade.heights.size() != size_t(shade.side) * shade.side || !shade.side) return shade_.Get();
    if (!shade_ || shade_->GetDesc().Width != shade.side) {
        if (shade_) Release(std::move(shade_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = TextureDesc(DXGI_FORMAT_R32_FLOAT, shade.side, shade.side);
        shade_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, shade_state_, nullptr,
                                                   IID_PPV_ARGS(&shade_));
        if (FAILED(hr)) {
            LOGE("D3D12: no %ux%u terrain shade", shade.side, shade.side);
            return nullptr;
        }
    }
    std::vector<uint8_t> rows(shade.heights.size() * sizeof(float));
    std::memcpy(rows.data(), shade.heights.data(), rows.size());
    Transition(shade_.Get(), shade_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    UploadRows(shade_.Get(), 0, rows, shade.side * sizeof(float));
    Transition(shade_.Get(), shade_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return shade_.Get();
}

bool D3D12Backend::EnsureFogTargets(uint32_t w, uint32_t h) {
    if (fog_[kFogTargets - 1] && fog_w_ == w && fog_h_ == h) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kGiFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    fog_w_ = fog_h_ = 0;
    for (int i = 0; i < kFogTargets; i++) {
        if (fog_[i]) Release(std::move(fog_[i]));
        fog_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, fog_state_[i], nullptr,
                                                   IID_PPV_ARGS(&fog_[i]));
        if (FAILED(hr)) {
            LOGE("D3D12: no %ux%u volumetric fog target", w, h);
            return false;
        }
        dev_->CreateRenderTargetView(fog_[i].Get(), nullptr, Rtv(kFogRtv + i));
    }
    fog_w_ = w;
    fog_h_ = h;
    return true;
}

// Volumetric clouds

// clouds marched at half size, then blended into the bound colour target where its depth holds only sky
void D3D12Backend::VolumetricClouds(const State& s, const OcclusionArgs& a) {
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples) return;
    uint32_t w = (rt->w + 1) / 2;
    uint32_t h = (rt->h + 1) / 2;
    if (!EnsureCloudTarget(w, h)) return;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    D3D12_RECT rect = HostRect(GuestScissor(s, b.w, b.h), b.scale);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    CloudView view = CloudViewAt(a.method);
    float c[kAoConstants];
    OcclusionConstants(a, d, rect, rt->w, rt->h, c);
    c[16] = float(w);
    c[17] = float(h);
    std::copy(std::begin(view.to_world), std::end(view.to_world), c + 32);
    std::copy(std::begin(view.sun), std::end(view.sun), c + 44);
    std::copy(std::begin(view.sky), std::end(view.sky), c + 48);
    std::copy(std::begin(view.sun_colour), std::end(view.sun_colour), c + 52);
    std::copy(std::begin(view.weather), std::end(view.weather), c + 56);
    AoPass march{.entry = "cloud_march", .out = kGiFormat, .rtv = Rtv(kCloudRtv), .clouds = true, .half = true};
    AoPass apply{.entry = "cloud_apply", .out = rt->fmt, .samples = rt->samples, .blend = true, .rtv = Rtv(rt->view),
                 .src = clouds_.Get(), .src_format = kGiFormat, .clouds = true};
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(clouds_.Get(), clouds_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool ok = OcclusionPass(march, *ds, c, rect);
    Transition(clouds_.Get(), clouds_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ok) OcclusionPass(apply, *ds, c, rect);
    Unbind();
}

// never shrinks: the water's reflections and the camera's pass take turns each frame
bool D3D12Backend::EnsureCloudTarget(uint32_t w, uint32_t h) {
    if (clouds_ && clouds_w_ >= w && clouds_h_ >= h) return true;
    w = std::max(w, clouds_w_);
    h = std::max(h, clouds_h_);
    if (clouds_) Release(std::move(clouds_));
    clouds_w_ = clouds_h_ = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kGiFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clouds_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, clouds_state_, nullptr,
                                               IID_PPV_ARGS(&clouds_));
    if (FAILED(hr)) {
        LOGE("D3D12: no %ux%u volumetric clouds target", w, h);
        return false;
    }
    dev_->CreateRenderTargetView(clouds_.Get(), nullptr, Rtv(kCloudRtv));
    clouds_w_ = w;
    clouds_h_ = h;
    return true;
}

// Caustics

// the caustics' light over what the camera's pass drew so far, which the water's draws take; under the sea it goes
// straight into the bound colour target
void D3D12Backend::Caustics(const State& s, const OcclusionArgs& a) {
    caustics_lit_ = false;
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples) return;
    CausticsView view = CausticsViewAt(a.method);
    if (!view.under && !EnsureCausticsTarget(rt->w, rt->h)) return;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    D3D12_RECT rect = HostRect(GuestScissor(s, b.w, b.h), b.scale);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    Resolved* sun = view.light.shadowed ? SunMap(fcip::SunMapsDrawn().far_base) : nullptr;
    float c[kAoConstants];
    OcclusionConstants(a, d, rect, rt->w, rt->h, c);
    c[16] = sun ? 1.0f : 0.0f;
    std::copy(std::begin(view.toward_sun), std::end(view.toward_sun), c + 20);
    std::copy(std::begin(view.light.sun), std::end(view.light.sun), c + 32);
    std::copy(std::begin(view.light.height), std::end(view.light.height), c + 48);
    std::copy(std::begin(view.light.sun_colour), std::end(view.light.sun_colour), c + 52);
    std::copy(std::begin(view.world_x), std::end(view.world_x), c + 56);
    std::copy(std::begin(view.world_y), std::end(view.world_y), c + 60);
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (view.under) {
        AoPass under{.entry = "caustics_under", .out = rt->fmt, .samples = rt->samples, .rtv = Rtv(rt->view),
                     .rgb = true, .sun = sun, .caustics = true, .lighten = true};
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        OcclusionPass(under, *ds, c, rect);
    } else {
        AoPass light{.entry = "caustics_light", .out = kCausticsFormat, .rtv = Rtv(kCausticsRtv), .sun = sun,
                     .caustics = true};
        Transition(caustics_.Get(), caustics_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        caustics_lit_ = OcclusionPass(light, *ds, c, rect);
        Transition(caustics_.Get(), caustics_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        caustics_rt_key_ = MapKey(*rt);
    }
    Unbind();
}

bool D3D12Backend::EnsureCausticsTarget(uint32_t w, uint32_t h) {
    if (caustics_ && caustics_w_ == w && caustics_h_ == h) return true;
    if (caustics_) Release(std::move(caustics_));
    caustics_w_ = caustics_h_ = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kCausticsFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    caustics_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, caustics_state_, nullptr,
                                               IID_PPV_ARGS(&caustics_));
    if (FAILED(hr)) {
        LOGE("D3D12: no %ux%u caustics target", w, h);
        return false;
    }
    dev_->CreateRenderTargetView(caustics_.Get(), nullptr, Rtv(kCausticsRtv));
    caustics_w_ = w;
    caustics_h_ = h;
    return true;
}

// Global illumination

// the camera pass's colour target: the world's draws stream into the scene and every draw writes its albedo beside it
void D3D12Backend::BeginIllumination(const State& s) {
    gi_.capture = false;
    gi_.mode = 0;
    gi_.draws.clear();
    gi_.divided = 0;
    if (!divide_pso_) return;
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || !EnsureIlluminationTargets(*rt)) return;
    Transition(gi_.albedo.Get(), gi_.albedo_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float none[4] = {};
    cl_->ClearRenderTargetView(Rtv(kAlbedoRtv), none, 0, nullptr);
    gi_.rt_key = MapKey(*rt);
    gi_.ds_key = MapKey(*ds);
    gi_.capture = true;
    gi_.mode = kAlbedoWrite;
}

// the scene's light traced from the bound depth, blurred across and down, and added into the bound colour target
void D3D12Backend::Illumination(const State& s, const OcclusionArgs& a) {
    bool open = gi_.mode != 0;
    gi_.capture = false;
    gi_.mode = 0;
    if (!open) return;
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || MapKey(*rt) != gi_.rt_key || rt->w != ds->w || rt->h != ds->h) return;
    if (rt->samples != ds->samples || rt->samples != gi_.samples || rt->w != gi_.w || rt->h != gi_.h) return;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    D3D12_RECT rect = HostRect(GuestScissor(s, b.w, b.h), b.scale);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    DivideView(a);
    if (gi_.draws.empty() || !BuildCasterScene(gi_.draws, false, gi_.bottom, gi_.top)) return;
    ID3D12Resource* colour = Feedback(*rt);
    if (!colour) return;
    float c[kAoConstants];
    OcclusionConstants(a, d, rect, rt->w, rt->h, c);
    c[16] = kGiRadius;
    // an sRGB view or a float target reads linear colour
    bool encoded = rt->fmt == DXGI_FORMAT_R8G8B8A8_UNORM || rt->fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
    c[17] = encoded ? kGiGamma : 1.0f;
    std::copy(std::begin(gi_.ambient), std::end(gi_.ambient), c + 28);
    FogView view = FogViewAt(a.method);
    Resolved* sun = view.shadowed ? SunMap(fcip::SunMapsDrawn().far_base) : nullptr;
    c[31] = sun ? 1.0f : 0.0f;
    std::copy(std::begin(view.sun), std::end(view.sun), c + 32);
    std::copy(std::begin(view.sun_colour), std::end(view.sun_colour), c + 52);
    D3D12_GPU_VIRTUAL_ADDRESS scene = gi_.top->GetGPUVirtualAddress();
    AoPass trace{.entry = "gi_trace", .out = kGiFormat, .rtv = Rtv(kGiRtv), .scene = scene, .gi = true,
                 .colour = colour, .colour_format = rt->fmt, .sun = sun, .half = true};
    AoPass across{.entry = "gi_blur", .out = kGiFormat, .rtv = Rtv(kGiRtv + 1), .src = gi_.light[0].Get(),
                  .step_x = 1, .src_format = kGiFormat, .gi = true, .colour = colour, .colour_format = rt->fmt,
                  .half = true};
    AoPass down{.entry = "gi_apply", .out = rt->fmt, .samples = rt->samples, .rtv = Rtv(rt->view),
                .src = gi_.light[1].Get(), .step_y = 1, .src_format = kGiFormat, .rgb = true, .gi = true,
                .colour = colour, .colour_format = rt->fmt};
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(gi_.albedo.Get(), gi_.albedo_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(gi_.light[0].Get(), gi_.light_state[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool ok = OcclusionPass(trace, *ds, c, rect);
    Transition(gi_.light[0].Get(), gi_.light_state[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(gi_.light[1].Get(), gi_.light_state[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ok = ok && OcclusionPass(across, *ds, c, rect);
    Transition(gi_.light[1].Get(), gi_.light_state[1], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ok) OcclusionPass(down, *ds, c, rect);
    Unbind();
    if (cap_) {
        char name[32];
        std::snprintf(name, sizeof(name), "%04u_albedo", cap_i_++);
        CapSave(gi_.albedo.Get(), gi_.albedo_state, kAlbedoFormat, gi_.samples, false, gi_.w, gi_.h, 1, name);
        std::snprintf(name, sizeof(name), "%04u_bounce", cap_i_++);
        CapSave(gi_.light[1].Get(), gi_.light_state[1], kGiFormat, 1, false, LightW(), LightH(), 1, name);
        // the share of rays reaching the sky, as grey
        std::snprintf(name, sizeof(name), "%04u_sky", cap_i_++);
        UINT alpha = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 3, 3, 3);
        CapSave(gi_.light[1].Get(), gi_.light_state[1], kGiFormat, 1, false, LightW(), LightH(), 1, name, alpha);
        const float* amb = gi_.ambient;
        std::fprintf(cap_, "illumination: %zu draws in the scene, ambient %g %g %g\n", gi_.draws.size(), amb[0],
                     amb[1], amb[2]);
    }
}

// the albedo at the colour target's size and samples, the light at half its size
bool D3D12Backend::EnsureIlluminationTargets(const Target& rt) {
    if (gi_.albedo && gi_.w == rt.w && gi_.h == rt.h && gi_.samples == rt.samples) return true;
    gi_.w = 0;
    gi_.h = 0;
    gi_.samples = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource>* res[3] = {&gi_.albedo, &gi_.light[0], &gi_.light[1]};
    D3D12_RESOURCE_STATES* state[3] = {&gi_.albedo_state, &gi_.light_state[0], &gi_.light_state[1]};
    for (uint32_t i = 0; i < 3; i++) {
        D3D12_RESOURCE_DESC d = TextureDesc(kAlbedoFormat, rt.w, rt.h);
        if (i) d = TextureDesc(kGiFormat, (rt.w + 1) / 2, (rt.h + 1) / 2);
        d.SampleDesc.Count = i ? 1 : rt.samples;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (*res[i]) Release(std::move(*res[i]));
        *state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, *state[i], nullptr,
                                                 IID_PPV_ARGS(&*res[i])))) {
            LOGE("D3D12: no %ux%u global illumination target", rt.w, rt.h);
            return false;
        }
        dev_->CreateRenderTargetView(res[i]->Get(), nullptr, Rtv(kAlbedoRtv + i));
    }
    gi_.w = rt.w;
    gi_.h = rt.h;
    gi_.samples = rt.samples;
    return true;
}

// a draw writing its colour unblended over a tested depth while the camera pass keeps an albedo
bool D3D12Backend::AlbedoWanted(const State& s, const ShaderInfo& ps) const {
    if (!gi_.mode || !(ps.exports & 1) || !Bits(s.Reg(kRbColorMask), 0, 4)) return false;
    bool blended = (s.Reg(kRbBlendControl0) & 0x1FFF1FFF) != 0x00010001;
    return Bits(s.Reg(kRbDepthControl), 1, 1) && !blended;
}

// the pixel shader writes the albedo, from the first of its 2D colour textures it fetches
void D3D12Backend::AlbedoFlags(const State& s, const ShaderInfo& ps, ShaderOptions& opt) const {
    opt.tex_flags[fcip::kAlbedoOut] |= fcip::kAlbedoFetch;
    size_t count = std::min<size_t>(ps.textures.size(), fcip::kAlbedoOut);
    for (size_t i = 0; i < count; i++) {
        // shadow maps carry flags of their own
        if (ps.textures[i].dimension != uint8_t(xenos::Dimension::D2) || opt.tex_flags[i]) continue;
        uint32_t fetch[6];
        ReadFetch(s, ps.textures[i].fetch, fetch);
        if (AlbedoFormat(xenos::DecodeTextureFetch(fetch).format)) opt.tex_flags[i] |= fcip::kAlbedoFetch;
    }
}

// a colour target's view, a multisampled one read by sample
void D3D12Backend::TargetView(ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t samples, uint32_t slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = fmt;
    v.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(res, &v, ViewCpu(slot));
}

// Motion vectors

// the scene's colour target: its draws write motion beside it until EndMotion, paused over the ocean
void D3D12Backend::BeginMotion(const State& s) {
    motion_.open = false;
    motion_.active = false;
    MotionCamera camera;
    if (!TakeMotionCamera(camera)) return;
    Target* rt = ColorTarget(s, 0, TargetHeight(s));
    if (!rt || rt->samples != 1 || !EnsureMotionTargets(rt->w, rt->h)) {
        motion_.has_last = false;
        return;
    }
    motion_.camera = camera;
    float clip[16];
    ClipMatrix(camera, clip);
    MotionConstants c{};
    if (!motion_.has_last || !Reprojection(clip, motion_.last_clip, c.reproject)) {
        std::fill(std::begin(c.reproject), std::end(c.reproject), 0.0f);
        for (int i = 0; i < 4; i++) c.reproject[i * 5] = 1;
    }
    if (!motion_.has_last) upscale_.reset = true;
    if (cap_) {
        const float* p = camera.proj;
        const float* v = camera.view;
        const float* r = c.reproject;
        const char* last = motion_.has_last ? "with" : "no";
        std::fprintf(cap_, "motion begin: proj %g %g %g %g, view at %g %g %g, reproject %g %g %g %g, %s last\n",
                     p[0], p[5], p[10], p[14], v[12], v[13], v[14], r[0], r[5], r[10], r[15], last);
    }
    std::memcpy(motion_.last_clip, clip, sizeof(clip));
    std::memcpy(motion_.reproject, c.reproject, sizeof(c.reproject));
    motion_.has_last = true;
    for (uint32_t prev = 0; prev < 2; prev++) {
        c.prev = prev;
        motion_.consts[prev] = UploadConstants(&c, sizeof(c));
    }
    motion_history_.Begin();
    Transition(motion_.drawn.Get(), motion_.drawn_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float none[4] = {kNoMotion, kNoMotion, 0, 0};
    cl_->ClearRenderTargetView(Rtv(kMotionRtv), none, 0, nullptr);
    motion_.rt_key = MapKey(*rt);
    upscale_.color = motion_.rt_key;
    motion_.draws = 0;
    motion_.matched = 0;
    motion_.open = true;
    motion_.active = true;
}

void D3D12Backend::EndMotion(const State& s) {
    if (!std::exchange(motion_.open, false)) return;
    motion_.active = false;
    uint32_t height = TargetHeight(s);
    Target* rt = ColorTarget(s, 0, height);
    Target* ds = DepthTarget(s, height);
    if (!rt || !ds || MapKey(*rt) != motion_.rt_key || ds->samples != 1) return;
    if (ds->w != motion_.w || ds->h != motion_.h) return;
    upscale_.depth = MapKey(*ds);
    upscale_.camera = motion_.camera;
    Bound b{TargetWidth(s), height, rt->scale};
    DrawConstants d;
    FillDrawConstants(s, b, d);
    CompleteMotion(*ds, d);
    upscale_.motion = true;
    if (cap_) {
        std::fprintf(cap_, "motion: %u draws, %u matched to last frame\n", motion_.draws, motion_.matched);
        char name[32];
        std::snprintf(name, sizeof(name), "%04u_motion", cap_i_++);
        CapSave(motion_.done.Get(), motion_.done_state, kMotionFormat, 1, false, motion_.w, motion_.h, 50, name);
    }
}

bool D3D12Backend::EnsureMotionTargets(uint32_t w, uint32_t h) {
    if (motion_.done && motion_.w == w && motion_.h == h) return true;
    motion_.w = 0;
    motion_.h = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kMotionFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource>* res[2] = {&motion_.drawn, &motion_.done};
    D3D12_RESOURCE_STATES* state[2] = {&motion_.drawn_state, &motion_.done_state};
    for (int i = 0; i < 2; i++) {
        if (*res[i]) Release(std::move(*res[i]));
        *state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, *state[i], nullptr,
                                                 IID_PPV_ARGS(&*res[i])))) {
            LOGE("D3D12: no %ux%u motion vector target", w, h);
            return false;
        }
        dev_->CreateRenderTargetView(res[i]->Get(), nullptr, Rtv(kMotionRtv + i));
    }
    motion_.w = w;
    motion_.h = h;
    return true;
}

// a draw that writes depth and its colour unblended
bool D3D12Backend::MotionWanted(const State& s, const ShaderInfo& ps) const {
    if (!motion_.active || !(ps.exports & 1) || !Bits(s.Reg(kRbColorMask), 0, 4)) return false;
    uint32_t depth = s.Reg(kRbDepthControl);
    bool writes_depth = Bits(depth, 1, 1) && Bits(depth, 2, 1);
    bool blended = (s.Reg(kRbBlendControl0) & 0x1FFF1FFF) != 0x00010001;
    return writes_depth && !blended;
}

// the draw's vertex constants last frame when a draw of the same signature and object matches, else its reprojection
void D3D12Backend::BindMotion(const State& s, const DrawArgs& a, const ShaderInfo& vs) {
    constexpr uint32_t kObjectPosition = 128;  // the render item's position, 825019E0
    constexpr uint32_t kPositionBit = kObjectPosition / 2;
    uint64_t stream = 0;
    uint64_t first = 0;
    if (!vs.fetches.empty() && a.streams) {
        stream = uint64_t(uintptr_t(a.streams[0].data));
        first = a.streams[0].first;
    }
    const uint64_t parts[6] = {uint64_t(uintptr_t(s.vs)), uint64_t(uintptr_t(a.indices)),
                               uint64_t(a.count) << 32 | a.start, uint64_t(uint32_t(a.base_vertex)), stream, first};
    uint64_t signature = Hash64(parts, sizeof(parts));
    float pos[3];
    bool placed = vs.constants[kPositionBit / 64] >> (kPositionBit % 64) & 1;
    const uint8_t* at = s.consts + kObjectPosition * 16;
    for (uint32_t i = 0; i < 3 && placed; i++) pos[i] = std::bit_cast<float>(Be32(at + i * 4));
    const MotionHistory::Draw& now = motion_history_.Record(signature, placed ? pos : nullptr, vs.constants, s.consts);
    const MotionHistory::Draw* last = motion_history_.Match(now);
    D3D12_GPU_VIRTUAL_ADDRESS consts = const_gpu_[kRootVsConsts];
    if (last) {
        Alloc up = Upload(kConstBytes, kCbAlign);
        motion_history_.Copy(*last, up.cpu);
        consts = up.gpu;
        motion_.matched++;
        if (cap_) CapMotionMatch(vs.constants, s.consts, up.cpu, placed);
    } else if (cap_) {
        std::fprintf(cap_, "     motion %s: no match, reprojected as still\n", placed ? "placed" : "in order");
    }
    motion_.draws++;
    if (Latch(binds_.roots[kRootPrevConsts], consts)) cl_->SetGraphicsRootConstantBufferView(kRootPrevConsts, consts);
    D3D12_GPU_VIRTUAL_ADDRESS mc = motion_.consts[last ? 1 : 0];
    if (Latch(binds_.roots[kRootMotion], mc)) cl_->SetGraphicsRootConstantBufferView(kRootMotion, mc);
}

// F3: how far the last frame's constants a draw was matched to are from its own
void D3D12Backend::CapMotionMatch(const uint64_t (&read)[2], const uint8_t* now_be, const uint8_t* last, bool placed) {
    uint32_t blocks = 0;
    uint32_t differ = 0;
    float most = 0;
    uint32_t most_at = 0;
    for (uint32_t w = 0; w < 2; w++) {
        for (uint64_t m = read[w]; m; m &= m - 1) {
            uint32_t block = w * 64 + uint32_t(std::countr_zero(m));
            bool same = true;
            for (uint32_t i = 0; i < 8; i++) {
                uint32_t at = block * 32 + i * 4;
                float now = std::bit_cast<float>(Be32(now_be + at));
                float was = 0;
                std::memcpy(&was, last + at, 4);
                float d = std::fabs(now - was);
                if (d == 0) continue;
                same = false;
                if (d > most) {
                    most = d;
                    most_at = at / 16;
                }
            }
            blocks++;
            differ += same ? 0 : 1;
        }
    }
    std::fprintf(cap_, "     motion match %s: %u of %u blocks differ, most %g at c%u\n", placed ? "placed" : "in order",
                 differ, blocks, most, most_at);
}

// kMotionHlsl into the completed target; d maps the title's ndc over the depth target as its draws did
void D3D12Backend::CompleteMotion(Target& depth, const DrawConstants& d) {
    ID3D12PipelineState* pso = EnsurePassPipeline(kMotionHlsl, "vs", "motion", kMotionFormat, -1);
    if (!pso) return;
    Transition(depth.res.Get(), depth.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(motion_.drawn.Get(), motion_.drawn_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(motion_.done.Get(), motion_.done_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t views = AllocViews(kPsTextures);
    ShaderView(depth.res.Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, views);
    ShaderView(motion_.drawn.Get(), kMotionFormat, views + 1);
    NullViews(views + 2, kPsTextures - 2);
    float c[24];
    std::memcpy(c, motion_.reproject, sizeof(motion_.reproject));
    // as OcclusionConstants: a pixel centre's host ndc back through the viewport to the title's
    const float rest[8] = {2.0f / float(motion_.w) / d.ndc_scale[0], -2.0f / float(motion_.h) / d.ndc_scale[1],
                           (-1 - d.ndc_offset[0]) / d.ndc_scale[0], (1 - d.ndc_offset[1]) / d.ndc_scale[1],
                           d.ndc_scale[2], d.ndc_offset[2], d.ndc_scale[0] * 0.5f, d.ndc_scale[1] * -0.5f};
    std::memcpy(c + 16, rest, sizeof(rest));
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Rtv(kMotionRtv + 1);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(motion_.w), float(motion_.h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(motion_.w), LONG(motion_.h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootFlow, UploadConstants(c, sizeof(c)));
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
    Unbind();
}

// FSR

uint64_t D3D12Backend::MapKey(const Target& t) const {
    return TargetMapKey(t.surface, t.depth, t.width, t.key) | (SceneScaled(t.width) ? kSceneTarget : 0);
}

// the render scale without FSR; with it the output's, as the scene alone is drawn at the render resolution
float D3D12Backend::BaseScale() const {
    float scale = app::RenderScale(guest_h_);
    if (!Fsr()) return scale;
    // the output's height in whole pixels, so the upscaled targets match the back buffer
    float out_h = std::round(scale * float(guest_h_) * 100.0f / float(app::Get().render_percent));
    return out_h / float(guest_h_);
}

float D3D12Backend::SceneScale() const {
    if (!Fsr()) return BaseScale();
    uint32_t percent = std::clamp(app::Get().render_percent, fsr::kMinPercent, fsr::kMaxPercent);
    // whole scene pixels in height, so a mode's own size lands exactly (a third of 2160 is 720)
    float out_h = BaseScale() * float(guest_h_);
    return std::round(out_h * fsr::SceneFraction(percent)) / float(guest_h_);
}

// the camera's pass begins: its full screen targets at the scene's scale, its draws offset by the next jitter
void D3D12Backend::BeginScene() {
    upscale_.scene = Fsr();
    upscale_.motion = false;
    if (!upscale_.scene) return;
    // Halton 2 and 3, eight phases for each output pixel a scene pixel covers
    float ratio = scale_ / scene_scale_;
    uint32_t phases = uint32_t(std::ceil(8 * ratio * ratio));
    uint32_t i = upscale_.jitters++ % phases + 1;
    jitter_[0] = Halton(i, 2) - 0.5f;
    jitter_[1] = Halton(i, 3) - 0.5f;
}

// what the last camera passes did, logged when it changes
void LogUpscale(const std::string& what) {
    static std::string last;
    if (what == last) return;
    last = what;
    LOGR("FSR: %s", what.c_str());
}

// the camera's pass ends: its colour upscaled into the same surface at the render scale, which the title draws on
void D3D12Backend::EndScene() {
    if (!std::exchange(upscale_.scene, false)) return;
    auto color = targets_.find(upscale_.color);
    if (color == targets_.end()) {
        LogUpscale("the camera pass drew no scene target");
        return;
    }
    Target& in = color->second;
    Target* out = EnsureTarget(in.surface, false, in.width, in.height);
    if (!out || !EnsureUpscaled(out->w, out->h)) return;
    auto depth = targets_.find(upscale_.depth);
    bool ok = false;
    if (upscale_.motion && depth != targets_.end()) {
        Target& ds = depth->second;
        fsr::Frame f;
        f.color = {in.res.Get(), uint32_t(ReadFormat(in)), in.w, in.h};
        f.depth = {ds.res.Get(), uint32_t(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS), ds.w, ds.h};
        f.motion = {motion_.done.Get(), 0, motion_.w, motion_.h};
        f.output = {upscale_.out.Get(), 0, upscale_.w, upscale_.h};
        f.camera = upscale_.camera;
        f.jitter[0] = jitter_[0];
        f.jitter[1] = jitter_[1];
        f.reset = upscale_.reset;
        f.index = presents_;
        if (cap_) {
            CapFinite(in.res.Get(), in.state, "FSR colour");
            CapFinite(ds.res.Get(), ds.state, "FSR depth");
            CapFinite(motion_.done.Get(), motion_.done_state, "FSR motion");
        }
        Transition(in.res.Get(), in.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        Transition(motion_.done.Get(), motion_.done_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        Transition(upscale_.out.Get(), upscale_.out_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ok = fsr::Upscale(cl_.Get(), f);
        if (ok && generation_swap_) {
            // frames are generated from gameplay FSR drew, never over the F10 panel
            ConfigureGeneration(!fcip::MenuOpen());
            fsr::Prepare(cl_.Get(), f);
        }
        Begin();  // FSR's passes left their heaps and root signature bound
        if (cap_) {
            CapFinite(upscale_.out.Get(), upscale_.out_state, "FSR output");
            char name[32];
            std::snprintf(name, sizeof(name), "%04u_fsr", cap_i_++);
            CapSave(upscale_.out.Get(), upscale_.out_state, kUpscaledFormat, 1, false, upscale_.w, upscale_.h, 1, name);
        }
    }
    char what[96];
    if (ok) std::snprintf(what, sizeof(what), "upscaling %ux%u to %ux%u", in.w, in.h, out->w, out->h);
    else std::snprintf(what, sizeof(what), "not upscaling, %s", upscale_.motion ? "FSR failed" : "no motion vectors");
    LogUpscale(what);
    upscale_.reset = !ok;
    if (!ok) DrawUpscaled(in.res.Get(), in.state, ReadFormat(in), 1, *out);
    else if (!DrawSharpened(in, *out)) DrawUpscaled(upscale_.out.Get(), upscale_.out_state, kUpscaledFormat, 1, *out);
}

// once a frame before its present through frame generation's swap chain
void D3D12Backend::ConfigureGeneration(bool enabled) {
    upscale_.configured = presents_;
    fsr::Generation g;
    g.swap = swap_.Get();
    g.index = presents_;
    g.enabled = enabled;
    std::copy(std::begin(frame_rect_), std::end(frame_rect_), g.rect);
    g.w = swap_w_;
    g.h = swap_h_;
    g.format = hdr_out_ ? kHdr10Format : kSwapFormat;
    g.hdr = hdr_out_;
    if (enabled && EnsureHudless()) {
        g.hudless = hudless_.out[presents_ % 2].Get();
        hudless_.used = presents_;
    }
    fsr::Configure(g);
}

// the target as the post effects leave it, before the HUD draws over it
void D3D12Backend::KeepHudless(const State& s) {
    if (!generation_swap_) return;
    Target* rt = ColorTarget(s, 0, TargetHeight(s));
    if (!rt || rt->samples != 1) return;
    D3D12_RESOURCE_DESC d = rt->res->GetDesc();
    D3D12_RESOURCE_DESC have{};
    if (hudless_.frame) have = hudless_.frame->GetDesc();
    if (have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
        if (hudless_.frame) Release(std::move(hudless_.frame));
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        hudless_.frame_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, hudless_.frame_state, nullptr,
                                                 IID_PPV_ARGS(&hudless_.frame)))) {
            LOGE("D3D12: no frame without the HUD");
            return;
        }
    }
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(hudless_.frame.Get(), hudless_.frame_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(hudless_.frame.Get(), rt->res.Get());
    hudless_.frame_format = ReadFormat(*rt);
    hudless_.kept = presents_;
}

// the back buffer's size and format, made again when they change
bool D3D12Backend::EnsureHudless() {
    DXGI_FORMAT format = hdr_out_ ? kHdr10Format : kSwapFormat;
    if (hudless_.out[0] && hudless_.w == swap_w_ && hudless_.h == swap_h_ && hudless_.format == format) return true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(format, swap_w_, swap_h_);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        if (hudless_.out[i]) Release(std::move(hudless_.out[i]));
        hudless_.out_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, hudless_.out_state[i], nullptr,
                                                 IID_PPV_ARGS(&hudless_.out[i])))) {
            LOGE("D3D12: no %ux%u back buffer without the HUD", swap_w_, swap_h_);
            hudless_.w = 0;
            return false;
        }
        dev_->CreateRenderTargetView(hudless_.out[i].Get(), nullptr, Rtv(kHudlessRtv + i));
    }
    hudless_.w = swap_w_;
    hudless_.h = swap_h_;
    hudless_.format = format;
    return true;
}

// the frame kept before the HUD as the back buffer shows the front buffer, else the front buffer itself
void D3D12Backend::DrawHudless(const xenos::TextureFetch& t) {
    Begin();  // RenderUi left ImGui's heap and root signature bound
    auto front = resolved_.find(ResolvedKey(t.base, 0));
    bool kept = hudless_.kept == presents_ && front != resolved_.end();
    uint32_t i = presents_ % 2;
    D3D12_CPU_DESCRIPTOR_HANDLE out = Rtv(kHudlessRtv + i);
    D3D12_CPU_DESCRIPTOR_HANDLE frame = out;
    DXGI_FORMAT frame_format = kSwapFormat;
    float black[4] = {0, 0, 0, 1};
    Transition(hudless_.out[i].Get(), hudless_.out_state[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(out, black, 0, nullptr);
    if (hdr_out_) {
        frame = Rtv(kOutRtv + kOutFrame);
        frame_format = kOutFrameFormat;
        Transition(out_[kOutFrame].Get(), out_state_[kOutFrame], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl_->ClearRenderTargetView(frame, black, 0, nullptr);
    }
    cl_->OMSetRenderTargets(1, &frame, FALSE, nullptr);
    if (kept) {
        // the title's shaders wrote the target in rgba order, which the front buffer's resolve and fetch undo
        const Resolved& r = front->second;
        uint32_t w = uint32_t(std::ceil(r.width * r.scale));
        uint32_t h = uint32_t(std::ceil(r.height * r.scale));
        BlitToSwap(hudless_.frame.Get(), hudless_.frame_state, hudless_.frame_format, frame_format, frame,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, w, h);
    } else {
        DrawFront(t, frame_format, frame);
    }
    if (hdr_out_) {
        Transition(out_[kOutFrame].Get(), out_state_[kOutFrame], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        ID3D12Resource* const src[3] = {out_[kOutFrame].Get(), nullptr, nullptr};
        const DXGI_FORMAT fmt[3] = {kOutFrameFormat, kSwapFormat, kSwapFormat};
        float p[8] = {paper_nits_, peak_nits_, 0, 0, 0, 0, 0, 0};
        Pass(kHdrHlsl, "hdr10", kHdr10Format, out, swap_w_, swap_h_, src, fmt, p, -1);
    }
    Transition(hudless_.out[i].Get(), hudless_.out_state[i], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

// frame generation's swap chain made or dropped as the setting changes
void D3D12Backend::RecreateSwapChain() {
    Submit(true);
    for (auto& b : bb_) b.Reset();
    if (generation_swap_) fsr::DestroySwapChain(swap_.Get());
    swap_.Reset();
    if (!CreateSwapChain()) return;
    // ResizeSwap makes the back buffers' views and colour space again
    swap_w_ = 0;
    hdr_want_ = false;
    LOGR("FSR: frame generation's swap chain %s", generation_swap_ ? "in use" : "dropped");
}

bool D3D12Backend::EnsureUpscaled(uint32_t w, uint32_t h) {
    if (upscale_.out && upscale_.w == w && upscale_.h == h) return true;
    if (upscale_.out) Release(std::move(upscale_.out));
    upscale_.w = 0;
    upscale_.h = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kUpscaledFormat, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    upscale_.out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, upscale_.out_state, nullptr,
                                             IID_PPV_ARGS(&upscale_.out)))) {
        LOGE("D3D12: no %ux%u FSR output", w, h);
        return false;
    }
    upscale_.w = w;
    upscale_.h = h;
    return true;
}

// src over the whole of out, taps per side where src is larger
void D3D12Backend::DrawUpscaled(ID3D12Resource* src, D3D12_RESOURCE_STATES& state, DXGI_FORMAT fmt, uint32_t taps,
                                Target& out) {
    Transition(src, state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(out.res.Get(), out.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Rtv(out.view);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(out.w), float(out.h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(out.w), LONG(out.h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    const float whole[4] = {0, 0, 1, 1};
    Blit(src, fmt, whole, false, true, taps, 1, out.fmt);
}

// FSR's output into the scene's target through AMD CAS, alpha from the scene; false when its pipeline cannot be made
bool D3D12Backend::DrawSharpened(Target& scene, Target& out) {
    static const std::string hlsl = std::string(kCasDefines) + kFfxA + kCasLoad + kFfxCas + kCasPass;
    // CasSetup (ffx_cas.h) without scaling, at sharpness 0, AMD's default with the least ringing
    constexpr float kSharpness = 0.0f;
    float sharp = -1.0f / std::lerp(8.0f, 5.0f, kSharpness);
    // the scene was drawn jitter_ pixels over
    const float p[8] = {sharp, 0, 8, 0, jitter_[0] / scene.w, jitter_[1] / scene.h, 0, 0};
    Transition(upscale_.out.Get(), upscale_.out_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(scene.res.Get(), scene.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(out.res.Get(), out.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* const src[3] = {upscale_.out.Get(), scene.res.Get(), nullptr};
    const DXGI_FORMAT fmt[3] = {kUpscaledFormat, ReadFormat(scene), kSwapFormat};
    return Pass(hlsl.c_str(), "vs", "cas", out.fmt, Rtv(out.view), out.w, out.h, src, fmt, p, -1);
}

// RTAO

// the world's opaque draws from here to Occlusion stream out their triangles, each draw from a zeroed counter
void D3D12Backend::BeginRayScene() {
    ray_capture_ = false;
    if (!dev5_) return;
    if (!ray_vertices_ && !MakeBuffer(ray_vertices_, kRayVertices * kRayStride, D3D12_RESOURCE_STATE_COMMON)) return;
    if (!ray_counters_ && !MakeBuffer(ray_counters_, kRayDraws * kRayCounter, D3D12_RESOURCE_STATE_COMMON)) return;
    uint64_t bytes = kRayDraws * kRayCounter;
    Alloc zeros = Upload(bytes, kRayCounter);
    std::memset(zeros.cpu, 0, bytes);
    Transition(ray_counters_.Get(), ray_counters_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyBufferRegion(ray_counters_.Get(), 0, zeros.res, zeros.offset, bytes);
    ray_draws_.clear();
    ray_used_ = 0;
    ray_capture_ = true;
}

// vertices the draw streams out: opaque triangles writing depth, none for any other or past the scene's room
uint32_t D3D12Backend::RayVertices(const DrawArgs& a, const PipelineKey& k, D3D12_PRIMITIVE_TOPOLOGY topo,
                                   uint32_t count, bool alpha_test) const {
    if (!ray_capture_ || alpha_test || !Bits(k.depth_control, 2, 1) || (k.color_control & 0x10)) return 0;
    if (k.rt_formats[0] && (k.blend[0] & 0x1FFF1FFF) != 0x00010001) return 0;
    uint32_t vertices = TriangleVertices(a, topo, count, k.strip_cut != 0);
    if (!vertices) return 0;
    if (ray_draws_.size() < kRayDraws && ray_used_ + vertices <= kRayVertices) return uint32_t(vertices);
    static bool logged = false;
    if (!std::exchange(logged, true)) LOGR("D3D12: RTAO scene full, draws past it left out");
    return 0;
}

// the next draw writes its triangles after the last one's
void D3D12Backend::StreamOut(const DrawConstants& c, uint32_t vertices) {
    Transition(ray_vertices_.Get(), ray_vertices_state_, D3D12_RESOURCE_STATE_STREAM_OUT);
    Transition(ray_counters_.Get(), ray_counters_state_, D3D12_RESOURCE_STATE_STREAM_OUT);
    RayDraw& d = ray_draws_.emplace_back();
    d.first = ray_used_;
    d.count = vertices;
    std::copy(c.ndc_scale, c.ndc_scale + 3, d.ndc_scale);
    std::copy(c.ndc_offset, c.ndc_offset + 3, d.ndc_offset);
    ray_used_ += vertices;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW v{};
    v.BufferLocation = ray_vertices_->GetGPUVirtualAddress() + d.first * kRayStride;
    v.SizeInBytes = uint64_t(vertices) * kRayStride;
    v.BufferFilledSizeLocation = ray_counters_->GetGPUVirtualAddress() + (ray_draws_.size() - 1) * kRayCounter;
    cl_->SOSetTargets(0, 1, &v);
}

// one bottom level structure of the streamed out draws, each moved into view space as it is built, held once by the
// top level one the rays trace. With global illumination the world streamed out as its casters, the opaque ones taken
D3D12_GPU_VIRTUAL_ADDRESS D3D12Backend::BuildRayScene(const OcclusionArgs& a) {
    if (gi_.capture) {
        DivideView(a);
        return BuildCasterScene(gi_.draws, true, ray_bottom_, ray_top_) ? ray_top_->GetGPUVirtualAddress() : 0;
    }
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    if (ray_draws_.empty() || FAILED(cl_.As(&cl4))) return 0;
    Alloc transforms = Upload(ray_draws_.size() * kRayTransformBytes, 16);
    D3D12_GPU_VIRTUAL_ADDRESS vertices = ray_vertices_->GetGPUVirtualAddress();
    ray_geometry_.clear();
    for (size_t i = 0; i < ray_draws_.size(); i++) {
        const RayDraw& d = ray_draws_[i];
        RayTransform(a, d, transforms.cpu + i * kRayTransformBytes);
        D3D12_RAYTRACING_GEOMETRY_DESC& g = ray_geometry_.emplace_back();
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.Transform3x4 = transforms.gpu + i * kRayTransformBytes;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = d.count;
        g.Triangles.VertexBuffer = {vertices + d.first * kRayStride, kRayStride};
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    bottom.NumDescs = UINT(ray_geometry_.size());
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.pGeometryDescs = ray_geometry_.data();
    Transition(ray_vertices_.Get(), ray_vertices_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!BuildStructure(cl4.Get(), bottom, ray_bottom_)) return 0;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = 1;
    instance.Transform[1][1] = 1;
    instance.Transform[2][2] = 1;
    instance.InstanceMask = 0xFF;
    instance.AccelerationStructure = ray_bottom_->GetGPUVirtualAddress();
    Alloc held = Upload(sizeof(instance), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
    std::memcpy(held.cpu, &instance, sizeof(instance));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.NumDescs = 1;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.InstanceDescs = held.gpu;
    if (!BuildStructure(cl4.Get(), top, ray_top_)) return 0;
    return ray_top_->GetGPUVirtualAddress();
}

// in built into dst; the barriers keep it after the last reads of dst and the scratch, and before the next
bool D3D12Backend::BuildStructure(ID3D12GraphicsCommandList4* cl4,
                                  const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in,
                                  ComPtr<ID3D12Resource>& dst) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (!Grow(dst, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) return false;
    if (!Grow(ray_scratch_, info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON)) return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs = in;
    d.DestAccelerationStructureData = dst->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = ray_scratch_->GetGPUVirtualAddress();
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl4->ResourceBarrier(1, &uav);
    cl4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    cl4->ResourceBarrier(1, &uav);
    return true;
}

// r holds at least bytes, made a quarter bigger so a growing scene seldom makes it again
bool D3D12Backend::Grow(ComPtr<ID3D12Resource>& r, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    if (r && r->GetDesc().Width >= bytes) return true;
    if (r) Release(std::move(r));
    return MakeBuffer(r, bytes + bytes / 4, state);
}

bool D3D12Backend::MakeBuffer(ComPtr<ID3D12Resource>& r, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = BufferDesc(bytes);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    if (SUCCEEDED(hr)) return true;
    LOGE("D3D12: no %llu byte RTAO buffer", static_cast<unsigned long long>(bytes));
    return false;
}

// Ray traced shadows

bool D3D12Backend::CreateDividePipeline() {
    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = kDivideConstants;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = p;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    if (FAILED(dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&divide_root_)))) {
        return false;
    }
    std::vector<uint8_t> cs;
    if (!CompileHlsl(kDivideHlsl, L"cs_6_0", cs)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = divide_root_.Get();
    d.CS = {cs.data(), cs.size()};
    return SUCCEEDED(dev_->CreateComputePipelineState(&d, IID_PPV_ARGS(&divide_pso_)));
}

bool D3D12Backend::EnsureCasterBuffers() {
    D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON;
    if (!caster_records_ && !MakeBuffer(caster_records_, kCasterRecords * kCasterStride, common)) return false;
    if (!caster_counters_ && !MakeBuffer(caster_counters_, kCasterDraws * kRayCounter, common)) return false;
    return casters_ || MakeBuffer(casters_, kCasterDraws * kCasterBytes, common);
}

// records a shadow map draw or a camera pass world draw streams out, filling its caster; none without ray traced
// shadows or global illumination, for the volumetric fog's sun map, or past the frame's room. An alpha tested one, or
// one whose alpha covers samples, is traced through its first fetch only, the texture its shader opens with
uint32_t D3D12Backend::CasterVertices(const State& s, const DrawArgs& a, const PipelineKey& k,
                                      D3D12_PRIMITIVE_TOPOLOGY topo, uint32_t count, const Bound& b, bool alpha_test,
                                      CasterDraw& caster) {
    if (!divide_pso_ || !b.ds || !Bits(k.depth_control, 2, 1)) return 0;
    bool view = SceneCaster(k, b);
    bool shadow = !fog_map_ && fcip::Get().shadow_filter >= fcip::kFilterRay && ShadowWidth(b.ds->width);
    if (!view && !shadow) return 0;
    fcip::FirstFetch f = s.ps ? s.ps->first_fetch : fcip::FirstFetch{};
    bool coverage = view && !alpha_test && Bits(k.color_control, 4, 1);
    if (coverage && f.reg < 0) return 0;
    uint32_t vertices = TriangleVertices(a, topo, count, k.strip_cut != 0);
    if (!vertices || !EnsureCasterBuffers()) return 0;
    if (caster_slots_ >= kCasterDraws || casters_used_ + vertices > kCasterRecords) {
        static bool logged = false;
        if (view && !std::exchange(logged, true)) LOGR("D3D12: global illumination scene full, draws past it left out");
        if (shadow) stamp_refused_++;  // diag
        return 0;
    }
    caster.target = b.ds;
    caster.w = b.w;
    caster.h = b.h;
    caster.count = vertices;
    caster.stream = kStreamCaster;
    caster.view = view;
    caster.info[3] = 7;  // D3DCMP_ALWAYS
    if ((alpha_test || coverage) && f.reg >= 0) {
        // alpha to coverage keeps the samples whose alpha reaches their share: at least half for the pixel
        float ref = coverage ? 0.5f : s.RegF(kRbAlphaRef);
        uint32_t compare = coverage ? 6 : Bits(s.Reg(kRbColorControl), 0, 3);
        caster.alpha = true;
        caster.stream = kStreamCaster + 1 + uint32_t(f.reg);
        caster.info[1] = binds_.stages[1].table;  // the first fetch's view leads the pixel shader's table
        caster.info[2] = std::bit_cast<uint32_t>(ref);
        caster.info[3] = compare | uint32_t(f.x) << 4 | uint32_t(f.y) << 6;
    }
    // global illumination reads a hidden hit's colour from the same fetch
    if (view && f.reg >= 0 && FirstTextureAlbedo(s)) {
        caster.stream = kStreamCaster + 1 + uint32_t(f.reg);
        caster.info[1] = binds_.stages[1].table;
        caster.info[3] |= kCasterTextured | uint32_t(f.x) << 4 | uint32_t(f.y) << 6;
    }
    if (view) {
        caster.info[4] = MaterialColour(s);
        caster.info[5] = SunReceiver(s) ? 1 : 0;
        caster.info[6] = AmbientColour(s);
    }
    return vertices;
}

// the pixel shader's first texture, which its first fetch reads, is a colour map
bool D3D12Backend::FirstTextureAlbedo(const State& s) const {
    if (!s.ps || s.ps->info.textures.empty() || !s.ps->first_fetch.colour) return false;
    const auto& t = s.ps->info.textures[0];
    if (t.dimension != uint8_t(xenos::Dimension::D2)) return false;
    uint32_t fetch[6];
    ReadFetch(s, t.fetch, fetch);
    return AlbedoFormat(xenos::DecodeTextureFetch(fetch).format);
}

// a constant register's rgb from 0 to 1 as 0xRRGGBB
uint32_t PackColour(const uint8_t* reg) {
    uint32_t rgb = 0;
    for (uint32_t i = 0; i < 3; i++) {
        float c = std::clamp(std::bit_cast<float>(Be32(reg + i * 4)), 0.0f, 1.0f);
        rgb = rgb << 8 | uint32_t(std::lround(c * 255));
    }
    return rgb;
}

// the draw's material colour as 0xRRGGBB, white when its shader takes none
uint32_t D3D12Backend::MaterialColour(const State& s) const {
    if (!s.ps || !s.ps->material) return 0xFFFFFF;
    return PackColour(s.consts + kConstBytes + fcip::kMaterialColour * 16);
}

// the ambient the game lights the draw by as 0xRRGGBB with bit 24 set, 0 when its shaders take none
uint32_t D3D12Backend::AmbientColour(const State& s) const {
    if (s.vs && s.vs->ambient) return PackColour(s.consts + fcip::kVsAmbient * 16) | 1u << 24;
    if (s.ps && s.ps->ambient) return PackColour(s.consts + kConstBytes + fcip::kPsAmbient * 16) | 1u << 24;
    return 0;
}

// the draw's pixel shader reads the far sun map: the game's materials without it take no sun, indoors among them
bool D3D12Backend::SunReceiver(const State& s) const {
    uint32_t far_base = fcip::SunMapsDrawn().far_base;
    if (!s.ps || !far_base) return false;
    for (const TextureBinding& b : s.ps->info.textures) {
        if (b.fetch != fcip::kSunFetch || b.dimension > 1) continue;
        uint32_t fetch[6];
        ReadFetch(s, b.fetch, fetch);
        if (xenos::DecodeTextureFetch(fetch).base == far_base) return true;
    }
    return false;
}

// a camera pass draw of the world, opaque, alpha tested or depth only, while global illumination's scene is captured
bool D3D12Backend::SceneCaster(const PipelineKey& k, const Bound& b) const {
    if (!gi_.capture || MapKey(*b.ds) != gi_.ds_key) return false;
    if (!b.rt) return true;
    return MapKey(*b.rt) == gi_.rt_key && (k.blend[0] & 0x1FFF1FFF) == 0x00010001;
}

// the draw's records after the last one's, from a zeroed counter; c maps its ndc
void D3D12Backend::StreamOutCasters(CasterDraw caster, const DrawConstants& c) {
    if (!caster_slots_) {
        uint64_t bytes = kCasterDraws * kRayCounter;
        Alloc zeros = Upload(bytes, kRayCounter);
        std::memset(zeros.cpu, 0, bytes);
        Transition(caster_counters_.Get(), caster_counters_state_, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyBufferRegion(caster_counters_.Get(), 0, zeros.res, zeros.offset, bytes);
    }
    Transition(caster_records_.Get(), caster_records_state_, D3D12_RESOURCE_STATE_STREAM_OUT);
    Transition(caster_counters_.Get(), caster_counters_state_, D3D12_RESOURCE_STATE_STREAM_OUT);
    caster.info[0] = uint32_t(casters_used_);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW v{};
    v.BufferLocation = caster_records_->GetGPUVirtualAddress() + casters_used_ * kCasterStride;
    v.SizeInBytes = uint64_t(caster.count) * kCasterStride;
    v.BufferFilledSizeLocation = caster_counters_->GetGPUVirtualAddress() + caster_slots_ * kRayCounter;
    cl_->SOSetTargets(0, 1, &v);
    casters_used_ += caster.count;
    caster_slots_++;
    if (!caster.view) {
        caster_draws_.push_back(caster);
        return;
    }
    const float ndc[4] = {c.ndc_scale[0], c.ndc_scale[1], c.ndc_offset[0], c.ndc_offset[1]};
    std::copy(std::begin(ndc), std::end(ndc), caster.ndc);
    gi_.draws.push_back(caster);
}

void D3D12Backend::DropCasters(const Target* t) {
    std::erase_if(caster_draws_, [t](const CasterDraw& d) { return d.target == t; });
}

// the casters drawn into src since it last resolved or cleared, divided into the uv and depth of the map r makes and
// built into that map's scene
void D3D12Backend::BuildRayShadow(const Target& src, const ResolveArgs& r) {
    std::vector<CasterDraw>& draws = caster_build_;
    draws.clear();
    std::erase_if(caster_draws_, [&](const CasterDraw& d) {
        if (d.target != &src) return false;
        draws.push_back(d);
        return true;
    });
    if (draws.empty() || casters_built_ + draws.size() > kCasterDraws) return;
    Stamp(kStampScene);  // diag
    DivideCasters(draws, r);
    RayShadow& scene = ray_shadows_[r.dst_base];
    if (BuildCasterScene(draws, false, scene.bottom, scene.top)) scene.frame = frame_;
    Stamp(kStampOther);  // diag
}

// the draws' divided casters built into one scene: the instance id is its first caster, a geometry per draw after it.
// Opaque keeps only the draws without an alpha test, which take no caster
bool D3D12Backend::BuildCasterScene(const std::vector<CasterDraw>& draws, bool opaque,
                                    ComPtr<ID3D12Resource>& bottom_out, ComPtr<ID3D12Resource>& top_out) {
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    if (draws.empty() || FAILED(cl_.As(&cl4))) return false;
    uint32_t first = casters_built_;
    D3D12_GPU_VIRTUAL_ADDRESS records = caster_records_->GetGPUVirtualAddress();
    ray_geometry_.clear();
    for (const CasterDraw& d : draws) {
        if (opaque && d.alpha) continue;
        D3D12_RAYTRACING_GEOMETRY_DESC& g = ray_geometry_.emplace_back();
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = d.alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = d.count;
        g.Triangles.VertexBuffer = {records + uint64_t(d.info[0]) * kCasterStride, kCasterStride};
    }
    if (ray_geometry_.empty()) return false;
    if (!opaque) {
        casters_built_ += uint32_t(draws.size());
        uint64_t bytes = draws.size() * kCasterBytes;
        Alloc infos = Upload(bytes, kCasterBytes);
        for (size_t i = 0; i < draws.size(); i++) {
            std::memcpy(infos.cpu + i * kCasterBytes, draws[i].info, kCasterBytes);
        }
        Transition(casters_.Get(), casters_state_, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyBufferRegion(casters_.Get(), uint64_t(first) * kCasterBytes, infos.res, infos.offset, bytes);
    }
    Transition(casters_.Get(), casters_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(caster_records_.Get(), caster_records_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    bottom.NumDescs = UINT(ray_geometry_.size());
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.pGeometryDescs = ray_geometry_.data();
    if (!BuildStructure(cl4.Get(), bottom, bottom_out)) return false;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = 1;
    instance.Transform[1][1] = 1;
    instance.Transform[2][2] = 1;
    instance.InstanceID = first;
    instance.InstanceMask = 0xFF;
    instance.AccelerationStructure = bottom_out->GetGPUVirtualAddress();
    Alloc held = Upload(sizeof(instance), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
    std::memcpy(held.cpu, &instance, sizeof(instance));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.NumDescs = 1;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.InstanceDescs = held.gpu;
    return BuildStructure(cl4.Get(), top, top_out);
}

// each run of records over one target size divided into the map's uv and depth: the host's ndc spans the target's
// title pixels, and the resolve moves its rect to the texture's dst point
void D3D12Backend::DivideCasters(const std::vector<CasterDraw>& draws, const ResolveArgs& r) {
    BeginDivide();
    float tw = float(std::max(r.dst_pitch, 1u));
    float th = float(std::max(r.dst_height, 1u));
    for (size_t i = 0; i < draws.size();) {
        const CasterDraw& d = draws[i];
        uint32_t count = d.count;
        size_t end = i + 1;
        // a draw into another target between them breaks the run
        while (end < draws.size() && draws[end].w == d.w && draws[end].h == d.h &&
               draws[end].info[0] == d.info[0] + count) {
            count += draws[end].count;
            end++;
        }
        const float scale_offset[4] = {0.5f * float(d.w) / tw, -0.5f * float(d.h) / th,
                                       (0.5f * float(d.w) - float(r.rect.x1) + float(r.dst_x)) / tw,
                                       (0.5f * float(d.h) - float(r.rect.y1) + float(r.dst_y)) / th};
        DivideRecords(d.info[0], count, scale_offset, 0);
        i = end;
    }
    EndDivide();
}

// the camera pass's draws not yet divided, into the view space kAoHlsl's View() makes: a host clip position goes back
// through its draw's ndc mapping and the projection, linear in x, y and w, the distance
void D3D12Backend::DivideView(const OcclusionArgs& a) {
    if (gi_.divided == gi_.draws.size()) return;
    BeginDivide();
    for (size_t i = gi_.divided; i < gi_.draws.size();) {
        const CasterDraw& d = gi_.draws[i];
        uint32_t count = d.count;
        size_t end = i + 1;
        while (end < gi_.draws.size() && std::equal(d.ndc, d.ndc + 4, gi_.draws[end].ndc) &&
               gi_.draws[end].info[0] == d.info[0] + count) {
            count += gi_.draws[end].count;
            end++;
        }
        const float scale_offset[4] = {a.proj[0] / d.ndc[0], a.proj[1] / d.ndc[1],
                                       -a.proj[0] * (d.ndc[2] / d.ndc[0] + a.proj[2]),
                                       -a.proj[1] * (d.ndc[3] / d.ndc[1] + a.proj[3])};
        DivideRecords(d.info[0], count, scale_offset, 1);
        i = end;
    }
    EndDivide();
    gi_.divided = gi_.draws.size();
}

void D3D12Backend::BeginDivide() {
    Transition(caster_records_.Get(), caster_records_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl_->SetComputeRootSignature(divide_root_.Get());
    cl_->SetPipelineState(divide_pso_.Get());
    cl_->SetComputeRootUnorderedAccessView(0, caster_records_->GetGPUVirtualAddress());
}

// count records from first, in dispatches of at most kDivideChunk
void D3D12Backend::DivideRecords(uint32_t first, uint32_t count, const float scale_offset[4], uint32_t view) {
    for (uint32_t done = 0; done < count; done += kDivideChunk) {
        uint32_t n = std::min(count - done, kDivideChunk);
        uint32_t c[kDivideConstants] = {first + done, n};
        std::memcpy(c + 2, scale_offset, 4 * sizeof(float));
        c[6] = view;
        cl_->SetComputeRoot32BitConstants(1, kDivideConstants, c, 0);
        cl_->Dispatch((n + 63) / 64, 1, 1);
    }
}

void D3D12Backend::EndDivide() {
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = caster_records_.Get();
    cl_->ResourceBarrier(1, &uav);
    // the graphics pipeline is set again by the next draw
    Unbind();
}

// pixel shader fetches of a shadow map whose scene this frame built are traced, each flagged with its scene's slot
void D3D12Backend::RayShadows(const State& s, const ShaderInfo& info, ShaderOptions& opt) {
    if (ray_shadows_.empty() || fcip::Get().shadow_filter < fcip::kFilterRay) return;
    if (info.textures.size() > fcip::kRayFirstSlot) return;
    fcip::SunMaps sun = fcip::SunMapsDrawn();
    uint32_t bases[fcip::kRayScenes] = {};
    uint32_t used = 0;
    for (uint32_t i = 0; i < info.textures.size(); i++) {
        if (info.textures[i].dimension > 1) continue;
        uint32_t fetch[6];
        ReadFetch(s, info.textures[i].fetch, fetch);
        uint32_t base = xenos::DecodeTextureFetch(fetch).base;
        auto scene = ray_shadows_.find(base);
        if (scene == ray_shadows_.end() || scene->second.frame != frame_) continue;
        uint32_t slot = uint32_t(std::find(bases, bases + used, base) - bases);
        if (slot == fcip::kRayScenes) continue;
        if (slot == used) {
            bases[used++] = base;
            ray_scenes_[slot] = scene->second.top->GetGPUVirtualAddress();
            ray_spread_[slot] = RaySpread(base, sun);
        }
        opt.tex_flags[i] |= uint8_t(fcip::kShadowRay | slot << fcip::kRaySlotShift);
    }
    if (!used) return;
    Transition(casters_.Get(), casters_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(caster_records_.Get(), caster_records_state_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

// diag: a timestamp where the kind of GPU work changes, the frame's last always fits
void D3D12Backend::Stamp(uint8_t kind) {
    std::vector<uint8_t>& kinds = stamp_kinds_[frame_ % kFrames];
    size_t room = kind == kStampEnd ? kStamps : kStamps - 1;
    if (!stamp_heap_ || kind == stamp_kind_ || kinds.size() >= room) return;
    uint32_t at = uint32_t(frame_ % kFrames) * kStamps + uint32_t(kinds.size());
    cl_->EndQuery(stamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, at);
    kinds.push_back(kind);
    stamp_kind_ = kind;
}

// diag
void D3D12Backend::EndStamps() {
    Stamp(kStampEnd);
    const std::vector<uint8_t>& kinds = stamp_kinds_[frame_ % kFrames];
    if (kinds.empty()) return;
    uint32_t base = uint32_t(frame_ % kFrames) * kStamps;
    cl_->ResolveQueryData(stamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, UINT(kinds.size()), stamp_read_.Get(),
                          base * sizeof(uint64_t));
}

// diag: the frame this slot last recorded, its GPU done; logged once a second
void D3D12Backend::ReadStamps() {
    std::vector<uint8_t>& kinds = stamp_kinds_[frame_ % kFrames];
    stamp_kind_ = kStampEnd;
    uint32_t base = uint32_t(frame_ % kFrames) * kStamps;
    D3D12_RANGE range{base * sizeof(uint64_t), (base + kinds.size()) * sizeof(uint64_t)};
    uint64_t* t = nullptr;
    if (kinds.size() > 1 && stamp_hz_ && SUCCEEDED(stamp_read_->Map(0, &range, reinterpret_cast<void**>(&t)))) {
        double ms = 1000.0 / double(stamp_hz_);
        for (size_t i = 0; i + 1 < kinds.size(); i++) {
            if (kinds[i] < kStampEnd) stamp_ms_[kinds[i]] += double(t[base + i + 1] - t[base + i]) * ms;
        }
        D3D12_RANGE none{};
        stamp_read_->Unmap(0, &none);
        stamp_frames_++;
    }
    kinds.clear();
    auto now = std::chrono::steady_clock::now();
    if (now - stamp_start_ < std::chrono::seconds(1) || !stamp_frames_) return;
    double n = stamp_frames_;
    LOGR("gpu ms a frame: traced draws %.2f (%.0f), alpha tested traced draws %.2f (%.0f), shadow map draws %.2f, "
         "shadow scene builds %.2f, other %.2f | %.0fk caster triangles, %u casters refused, filter %u",
         stamp_ms_[kStampTraced] / n, stamp_draws_[0] / n, stamp_ms_[kStampTracedAlpha] / n, stamp_draws_[1] / n,
         stamp_ms_[kStampShadowMap] / n, stamp_ms_[kStampScene] / n, stamp_ms_[kStampOther] / n,
         double(stamp_records_) / 3 / n / 1000, stamp_refused_, fcip::Get().shadow_filter);
    stamp_draws_[0] = 0;
    stamp_draws_[1] = 0;
    std::fill(std::begin(stamp_ms_), std::end(stamp_ms_), 0.0);
    stamp_records_ = 0;
    stamp_refused_ = 0;
    stamp_frames_ = 0;
    stamp_start_ = now;
}

// an alpha tested draw that traces shadows, depth tested and written without stencil, blending or alpha to coverage:
// its depth goes first through the same shader without rays, then k tests equal without writing, so the traced pass
// shades only the pixels the draw shows. None while a query counts samples
ID3D12PipelineState* D3D12Backend::EnsureDepthPass(const State& s, const ShaderOptions& ps_opt, const Variant& vs,
                                                   PipelineKey& k) {
    constexpr uint32_t kZTestAndWrite = 6;  // RB_DEPTHCONTROL bits 0 to 2: stencil off, z on, z write on
    constexpr uint32_t kZWriteAndFunc = 0x74;  // z write bit 2, z func bits 4 to 6
    constexpr uint32_t kZEqual = 2 << 4;
    constexpr uint8_t kRayFlags = fcip::kShadowRay | (fcip::kRayScenes - 1) << fcip::kRaySlotShift;
    uint32_t c = k.depth_control;
    if (!ray_scenes_[0] || !ps_opt.alpha_test || open_queries_ || Bits(c, 0, 3) != kZTestAndWrite) return nullptr;
    if (Bits(k.color_control, 4, 1)) return nullptr;
    for (uint32_t i = 0; i < 4; i++) {
        if (k.rt_formats[i] && (k.blend[i] & 0x1FFF1FFF) != 0x00010001) return nullptr;
    }
    ShaderOptions opt = ps_opt;
    for (uint32_t i = 0; i < s.ps->info.textures.size(); i++) {
        if (opt.tex_flags[i] & fcip::kShadowRay) opt.tex_flags[i] &= uint8_t(~kRayFlags);
    }
    Variant* ps = EnsureVariant(s.ps, opt);
    if (!ps || ps->info.writes_depth) return nullptr;
    PipelineKey depth = k;
    depth.ps = ps->key;
    depth.stream_out = 0;
    depth.depth_only = 1;
    ID3D12PipelineState* pso = EnsurePipeline(depth, vs, ps);
    if (pso) k.depth_control = (c & ~kZWriteAndFunc) | kZEqual;
    return pso;
}

// the far sun map's rays spread over the sun's disc, its uv per unit of depth; another light's over a disc of the same
// world size at every shadow resolution, negative
float D3D12Backend::RaySpread(uint32_t base, const fcip::SunMaps& sun) const {
    if (base == sun.far_base) return kRaySunSpread * sun.depth_ratio;
    auto map = shadow_resolves_.find(base);
    bool half = map != shadow_resolves_.end() && map->second.width < shadow_side_;
    return -kRayDisc / (half ? kStockSide / 2 : kStockSide);
}

// the casters, their records and the scenes a draw traces, in its pixel shader's views from kRayFirstSlot
void D3D12Backend::RayViews(uint32_t table, const D3D12_GPU_VIRTUAL_ADDRESS* rays) {
    CasterViews(table + fcip::kRayFirstSlot);
    D3D12_SHADER_RESOURCE_VIEW_DESC scene{};
    scene.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    scene.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    for (uint32_t i = 0; i < fcip::kRayScenes; i++) {
        if (!rays[i]) continue;
        scene.RaytracingAccelerationStructure.Location = rays[i];
        dev_->CreateShaderResourceView(nullptr, &scene, ViewCpu(table + fcip::kRayFirstSlot + 2 + i));
    }
}

// the casters and their records, as CasterHit reads them, in views at and after at
void D3D12Backend::CasterViews(uint32_t at) {
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    raw.Buffer.NumElements = UINT(kCasterDraws * kCasterBytes / 4);
    dev_->CreateShaderResourceView(casters_.Get(), &raw, ViewCpu(at));
    raw.Buffer.NumElements = UINT(kCasterRecords * kCasterStride / 4);
    dev_->CreateShaderResourceView(caster_records_.Get(), &raw, ViewCpu(at + 1));
}

// D3D Resolve: the target's rect to the texture at dst_base, held on the host and read in place of guest memory
void D3D12Backend::Resolve(const State& s, const ResolveArgs& r) {
    Stamp(kStampOther);  // diag
    g_perf.resolves++;
    uint32_t height = std::max<uint32_t>(TargetHeight(s), uint32_t(r.rect.y2));
    Target* src = r.source < 4 ? ColorTarget(s, r.source, height) : DepthTarget(s, height);
    const xenos::FormatInfo& fi = xenos::Info(xenos::TexFormat(r.dst_format));
    if (src && src->range != 1 && ext_) ext_->Resolve(*this, *src);
    if (src && cap_) {
        std::fprintf(cap_, "%04u resolve %s %u to %08X format %u\n", cap_i_, src->depth ? "depth" : "colour",
                     src->surface.base, r.dst_base, r.dst_format);
        const Rect& p = s.predicated;
        std::fprintf(cap_, "     rect %d,%d,%d,%d at %d,%d size %ux%u tiles %d,%d,%d,%d\n", r.rect.x1, r.rect.y1,
                     r.rect.x2, r.rect.y2, r.dst_x, r.dst_y, r.dst_pitch, r.dst_height, p.x1, p.y1, p.x2, p.y2);
        char name[48];
        std::snprintf(name, sizeof(name), "%04u_resolve_%08X", cap_i_++, r.dst_base);
        CapSave(*src, name, 1);
    }
    if (src && fi.block == 1 && fi.convert != xenos::Convert::Unsupported) {
        uint32_t w = std::max(r.dst_pitch, 1u);
        uint32_t h = std::max(r.dst_height, 1u);
        uint64_t key = ResolvedKey(r.dst_base, src->key);
        Resolved& dst = EnsureResolved(key, fi.host, w, h, src->scale);
        if (dst.res) {
            CopyResolve(*src, dst, r);
            KeepBlocks(*src, dst, r);
        }
        latest_[r.dst_base] = key;
    } else if (src) {
        static bool logged = false;
        if (!std::exchange(logged, true)) LOGR("D3D12: resolve to format %u left out", r.dst_format);
    }
    {
        std::lock_guard lock(g_cpu.lock);
        uint64_t seq = ++g_cpu.done[r.dst_base];
        auto read = g_cpu.read.find(r.dst_base);
        if (read != g_cpu.read.end()) RecordReadBack(read->second, r.dst_base, seq);
    }
    g_cpu.ran.notify_all();
    fcip::SunResolved(r.dst_base);
    if (src && shadow_targets_.count(TargetMapKey(src->surface, src->depth, src->width, src->key))) {
        shadow_resolves_[r.dst_base] = {shadow_frame_, src->width};
        if (src->depth) BuildRayShadow(*src, r);
    }
    if (src && r.clear) {
        ClearTarget(*src, r.clear, r.clear_color, r.clear_z, uint8_t(r.clear_stencil), nullptr, 0);
        ClearSpan(*src, r.clear, r.clear_color, r.clear_z, uint8_t(r.clear_stencil));
    }
}

// the host texture read in place of guest memory, made again when the format or size changes
Resolved& D3D12Backend::EnsureResolved(uint64_t key, DXGI_FORMAT format, uint32_t w, uint32_t h, float scale) {
    Resolved& dst = resolved_[key];
    MakeResolved(dst, format, w, h, scale);
    return dst;
}

// w x h title texels at scale host texels each
void D3D12Backend::MakeResolved(Resolved& dst, DXGI_FORMAT format, uint32_t w, uint32_t h, float scale) {
    if (dst.res && dst.format == format && dst.width == w && dst.height == h && dst.scale == scale) return;
    if (dst.res) {
        Release(dst.res);
    } else if (!free_rtv_.empty()) {
        dst.view = free_rtv_.back();
        free_rtv_.pop_back();
    }
    dst.format = format;
    dst.width = w;
    dst.height = h;
    dst.scale = scale;
    uint32_t hw = uint32_t(std::ceil(w * scale));
    uint32_t hh = uint32_t(std::ceil(h * scale));
    D3D12_RESOURCE_DESC d = TextureDesc(format, hw, hh);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    dst.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, dst.state, nullptr, IID_PPV_ARGS(&dst.res));
    dev_->CreateRenderTargetView(dst.res.Get(), nullptr, Rtv(dst.view));
}

void D3D12Backend::ReleaseResolved(Resolved& r) {
    if (!r.res) return;
    Release(r.res);
    free_rtv_.push_back(r.view);
}

void D3D12Backend::DropTarget(uint64_t map_key) {
    auto it = targets_.find(map_key);
    if (it == targets_.end()) return;
    Target& t = it->second;
    if (&t == cap_rt_) cap_rt_ = nullptr;
    Release(t.res);
    Release(t.fb);
    (t.depth ? free_dsv_ : free_rtv_).push_back(t.view);
    targets_.erase(it);
    Unbind();
}

// a guest pitch a sun map side or half of it rounds to
bool FitsSide(uint32_t width, uint32_t side) {
    for (uint32_t s : {side, side / 2}) {
        if (s && width >= s && width < s + kPitchAlign) return true;
    }
    return false;
}

// a shadow map pitch of the side now or, for frames recorded before the last change, the one before
bool D3D12Backend::ShadowWidth(uint32_t width) const {
    bool grace = presents_ - side_changed_ < kSideGrace;
    return FitsSide(width, shadow_side_) || (grace && FitsSide(width, last_shadow_side_));
}

void D3D12Backend::KeepShadowTarget(const Target& t) {
    shadow_targets_[TargetMapKey(t.surface, t.depth, t.width, t.key)] = {shadow_frame_, t.width};
}

// shadow maps of a side the setting left, or that no shadow frame drew for a while; freed before the next frame
// makes others, so two sides never share VRAM
void D3D12Backend::DropIdleShadowMaps() {
    if (std::exchange(shadow_drawn_, false)) shadow_frame_++;
    auto unused = [&](const ShadowUse& u) {
        return shadow_frame_ - u.frame > kShadowIdleFrames || !FitsSide(u.width, shadow_side_);
    };
    size_t kept = shadow_targets_.size() + shadow_resolves_.size();
    std::erase_if(shadow_targets_, [&](const auto& entry) {
        if (!unused(entry.second)) return false;
        DropTarget(entry.first);
        return true;
    });
    std::erase_if(shadow_resolves_, [&](const auto& entry) {
        if (!unused(entry.second)) return false;
        DropResolved(entry.first);
        return true;
    });
    if (shadow_targets_.size() + shadow_resolves_.size() == kept) return;
    Submit(true);
    for (Frame& f : frames_) f.garbage.clear();
}

// guest memory at base is read again
void D3D12Backend::DropResolved(uint32_t base) {
    std::erase_if(resolved_, [&](auto& entry) {
        if (uint32_t(entry.first) != base) return false;
        ReleaseResolved(entry.second);
        return true;
    });
    if (auto it = blocks_.find(base); it != blocks_.end()) {
        ReleaseResolved(it->second);
        blocks_.erase(it);
    }
    latest_.erase(base);
    // a kept table may show the resolve
    Unbind();
}

// a rect not a multiple of 8 also writes the EDRAM past it into memory the CPU may read, which the texture lacks: kept
// beside it from then on, as RAM keeps it
void D3D12Backend::KeepBlocks(Target& src, Resolved& dst, const ResolveArgs& r) {
    ResolveArgs whole = r;
    whole.rect.x2 = (r.rect.x2 + kResolveBlock - 1) / kResolveBlock * kResolveBlock;
    whole.rect.y2 = (r.rect.y2 + kResolveBlock - 1) / kResolveBlock * kResolveBlock;
    uint32_t width = std::max(dst.width, uint32_t(r.dst_x + whole.rect.x2 - whole.rect.x1));
    uint32_t height = std::max(dst.height, uint32_t(r.dst_y + whole.rect.y2 - whole.rect.y1));
    auto kept = blocks_.find(r.dst_base);
    bool past = width > dst.width || height > dst.height;
    if (!past && kept == blocks_.end()) return;
    Resolved& block = blocks_[r.dst_base];
    width = std::max(width, block.width);
    height = std::max(height, block.height);
    MakeResolved(block, dst.format, width, height, dst.scale);
    if (!block.res) return;
    // the texture over what earlier resolves left past it, then this resolve's blocks
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(block.res.Get(), block.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION texture{dst.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION into{block.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    cl_->CopyTextureRegion(&into, 0, 0, 0, &texture, nullptr);
    if (past) CopyResolve(src, block, whole);
}

// the title's CPU reads src next, and a D3DX copy writes dst's top level: a resolve at src goes to guest memory first,
// and dst is read from guest memory again
void D3D12Backend::Copy(const CopyArgs& c) {
    WriteResolved(c.src);
    xenos::TextureFetch dst = xenos::DecodeTextureFetch(c.dst);
    std::unique_lock lock(g_cpu.lock);
    if (dst.base && !dst.mip_min) {
        DropResolved(dst.base);
        // resolves there count again from here, as QueueCopy counts them
        g_cpu.done.erase(dst.base);
        if (auto read = g_cpu.read.find(dst.base); read != g_cpu.read.end()) {
            for (ComPtr<ID3D12Resource>& buffer : read->second.buffer) {
                if (buffer) Release(std::move(buffer));
            }
            g_cpu.read.erase(read);
        }
    }
    g_cpu.run++;
    lock.unlock();
    g_cpu.ran.notify_all();
}

// title texels in host order into guest memory as the texture lays them out, as far as its level's memory reaches
void WriteTexels(const uint8_t* p, uint32_t row_pitch, uint32_t w, uint32_t h, const xenos::TextureFetch& t) {
    uint32_t bytes = xenos::Info(t.format).bytes;
    uint32_t log2 = uint32_t(std::countr_zero(bytes));
    xenos::Level l = xenos::Levels(t)[0];
    uint8_t* memory = const_cast<uint8_t*>(Guest(l.address));
    w = std::min(w, l.pitch_blocks);
    h = std::min(h, l.rows_blocks);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t texel[16];
            std::memcpy(texel, p + size_t(y) * row_pitch + size_t(x) * bytes, bytes);
            xenos::SwapEndian(texel, bytes, t.endian);
            uint32_t index = t.tiled ? xenos::TiledOffset2D(x, y, l.pitch_blocks, log2) : y * l.pitch_blocks + x;
            std::memcpy(memory + (size_t(index) << log2), texel, bytes);
        }
    }
}

// what RAM holds at base after its last resolve: the texture, or the texture with the blocks resolves wrote past it
Resolved* D3D12Backend::CpuSource(uint32_t base) {
    if (auto block = blocks_.find(base); block != blocks_.end()) return &block->second;
    auto latest = latest_.find(base);
    if (latest == latest_.end()) return nullptr;
    auto kept = resolved_.find(latest->second);
    return kept != resolved_.end() && kept->second.res ? &kept->second : nullptr;
}

// from at title size in cpu_texels_, each title texel the box of host texels it covers
bool D3D12Backend::MakeCpuTexels(Resolved& from) {
    MakeResolved(cpu_texels_, from.format, from.width, from.height, 1);
    if (!cpu_texels_.res || !from.res) return false;
    D3D12_RESOURCE_DESC host = from.res->GetDesc();
    Transition(from.res.Get(), from.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(cpu_texels_.res.Get(), cpu_texels_.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(cpu_texels_.view);
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(from.width), float(from.height), 0, 1};
    D3D12_RECT sc{0, 0, LONG(from.width), LONG(from.height)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    float uv[4] = {0, 0, from.width * from.scale / host.Width, from.height * from.scale / host.Height};
    Blit(from.res.Get(), from.format, uv, false, true, uint32_t(std::ceil(from.scale)), 1, from.format);
    return true;
}

// cpu_texels_ into a readback buffer, made or grown as needed
bool D3D12Backend::CopyCpuTexels(ComPtr<ID3D12Resource>& buffer, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
    D3D12_RESOURCE_DESC rd = cpu_texels_.res->GetDesc();
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (buffer && buffer->GetDesc().Width < bytes) Release(std::move(buffer));
    if (!buffer) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) return false;
    }
    Transition(cpu_texels_.res.Get(), cpu_texels_.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    to.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION texels{cpu_texels_.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    cl_->CopyTextureRegion(&to, 0, 0, 0, &texels, nullptr);
    return true;
}

// a resolve to an address the CPU reads: copied for the next lock there, unless the GPU is a whole ring behind
void D3D12Backend::RecordReadBack(ReadBack& rb, uint32_t base, uint64_t seq) {
    Resolved* from = CpuSource(base);
    auto latest = latest_.find(base);
    if (!from || latest == latest_.end() || !MakeCpuTexels(*from)) return;
    uint32_t slot = rb.next;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    if (rb.fence[slot] > fence_->GetCompletedValue() || !CopyCpuTexels(rb.buffer[slot], fp)) return;
    // the lock waits on this submission, not on the frame's
    Kick();
    const Resolved& resolve = resolved_[latest->second];
    rb.seq[slot] = seq;
    rb.fence[slot] = fence_value_;
    rb.next = (slot + 1) % kReadBacks;
    rb.format = resolve.format;
    rb.width = resolve.width;
    rb.height = resolve.height;
    rb.copy_width = from->width;
    rb.copy_height = from->height;
    rb.row_pitch = fp.Footprint.RowPitch;
}

// the host keeps these formats as the guest's bytes
bool CpuReadable(const xenos::TextureFetch& t) {
    const xenos::FormatInfo& fi = xenos::Info(t.format);
    if (fi.block == 1 && fi.convert == xenos::Convert::None) return true;
    static bool logged = false;
    if (!std::exchange(logged, true)) LOGR("D3D12: CPU read of a format %u resolve left out", uint32_t(t.format));
    return false;
}

// the last resolve to a texture the CPU reads: written to guest memory now, after the GPU finished it
void D3D12Backend::WriteResolved(const uint32_t fetch[6]) {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
    auto latest = latest_.find(t.base);
    if (t.mip_min || latest == latest_.end() || !CpuReadable(t)) return;
    // a texture of another size or format at the address replaced the resolve
    const Resolved& resolve = resolved_[latest->second];
    bool same = resolve.format == HostTextureFormat(t) && resolve.width == t.width && resolve.height == t.height;
    Resolved* from = CpuSource(t.base);
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    if (!same || !from || !MakeCpuTexels(*from) || !CopyCpuTexels(buffer, fp)) return;
    Submit(true);
    uint8_t* p = nullptr;
    if (SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
        WriteTexels(p, fp.Footprint.RowPitch, from->width, from->height, t);
        D3D12_RANGE none{0, 0};
        buffer->Unmap(0, &none);
    }
    Release(std::move(buffer));
}

void D3D12Backend::CopyResolve(Target& src, Resolved& dst, const ResolveArgs& r) {
    bool ms = src.samples > 1;
    ID3D12PipelineState* pso = EnsureResolvePipeline(dst.format, ms);
    if (!pso) return;
    uint32_t views = AllocViews(kPsTextures);
    NullViews(views + 1, kPsTextures - 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = src.depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : ReadFormat(src);
    v.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (r.swap && !src.depth) v.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, 3);
    v.Texture2D.MipLevels = 1;
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    dev_->CreateShaderResourceView(src.res.Get(), &v, ViewCpu(views));
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // source and destination each in their own host pixels
    float ss = src.scale;
    float cb[8] = {r.rect.x1 * ss, r.rect.y1 * ss, (r.rect.x2 - r.rect.x1) * ss, (r.rect.y2 - r.rect.y1) * ss,
                   float(src.samples), src.depth ? 1.0f : 0.0f, 0, 0};
    D3D12_GPU_VIRTUAL_ADDRESS addr = UploadConstants(cb, sizeof(cb));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(dst.view);
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float ds = dst.scale;
    float dw = (r.rect.x2 - r.rect.x1) * ds;
    float dh = (r.rect.y2 - r.rect.y1) * ds;
    D3D12_VIEWPORT vp{r.dst_x * ds, r.dst_y * ds, dw, dh, 0, 1};
    D3D12_RECT sc{0, 0, LONG(std::ceil(dst.width * ds)), LONG(std::ceil(dst.height * ds))};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootVsConsts, addr);
    cl_->SetGraphicsRootConstantBufferView(kRootPsConsts, addr);
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl_->DrawInstanced(4, 1, 0, 0);
    Unbind();
}

// Present

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    bool hdr = hdr_display_ && fcip::Get().hdr_output;
    if (w == swap_w_ && h == swap_h_ && hdr == hdr_want_) return;
    Submit(true);
    for (auto& b : bb_) b.Reset();
    hdr_want_ = hdr;
    hdr_out_ = hdr && ResizeBackBuffers(w, h, kHdr10Format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (!hdr_out_) ResizeBackBuffers(w, h, kSwapFormat, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (hdr && !hdr_out_) LOGR("Display HDR: the swap chain can't show HDR10, SDR it is");
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Rtv(kRtvCount + i));
    }
    swap_w_ = w;
    swap_h_ = h;
}

// false when the swap chain can't present in that colour space
bool D3D12Backend::ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space) {
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kFrames, w, h, fmt, flags))) return false;
    UINT support = 0;
    swap_->CheckColorSpaceSupport(space, &support);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return false;
    return SUCCEEDED(swap_->SetColorSpace1(space));
}

// the output the window's display has: a factory made before Windows HDR was switched no longer tells
void D3D12Backend::UpdateHdrDisplay() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    if (mon == hdr_mon_ && factory_->IsCurrent()) return;
    hdr_mon_ = mon;
    ComPtr<IDXGIFactory4> fresh;
    if (!factory_->IsCurrent() && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh)))) factory_ = fresh;
    DXGI_OUTPUT_DESC1 desc{};
    bool found = FindOutput(factory_.Get(), mon, desc);
    hdr_display_ = found && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    bool known_peak = found && desc.MaxLuminance > 0 && desc.MaxLuminance <= kMaxPeakNits;
    peak_nits_ = known_peak ? desc.MaxLuminance : kDefaultPeakNits;
    paper_nits_ = SdrWhiteNits(mon);
    LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr_display_ ? "on" : "off", peak_nits_,
         paper_nits_);
}

bool D3D12Backend::CreateOutTarget(int i, DXGI_FORMAT fmt) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(fmt, swap_w_, swap_h_);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    out_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, out_state_[i], nullptr,
                                             IID_PPV_ARGS(&out_[i])))) {
        LOGE("d3d12: no %ux%u HDR10 output target", swap_w_, swap_h_);
        return false;
    }
    dev_->CreateRenderTargetView(out_[i].Get(), nullptr, Rtv(kOutRtv + i));
    return true;
}

// the HDR10 output's targets at the back buffers' size
bool D3D12Backend::EnsureOutTargets() {
    if (out_[kOutFrame] && out_[kOutPanel] && out_w_ == swap_w_ && out_h_ == swap_h_) return true;
    for (auto& o : out_) {
        if (o) Release(std::move(o));
    }
    out_w_ = swap_w_;
    out_h_ = swap_h_;
    return CreateOutTarget(kOutFrame, kOutFrameFormat) && CreateOutTarget(kOutPanel, kSwapFormat);
}

// the front buffer, a resolve or a texture, into the bound target
void D3D12Backend::DrawFront(const xenos::TextureFetch& t, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE target) {
    auto resolved = resolved_.find(ResolvedKey(t.base, 0));
    if (resolved != resolved_.end()) {
        Resolved& r = resolved->second;
        uint32_t w = uint32_t(std::ceil(r.width * r.scale));
        uint32_t h = uint32_t(std::ceil(r.height * r.scale));
        BlitToSwap(r.res.Get(), r.state, r.format, rtv, target, ComponentMapping(t), w, h);
    } else if (Texture* tex = EnsureTexture(t)) {
        BlitToSwap(tex->res.Get(), tex->state, tex->format, rtv, target, ComponentMapping(t), t.width, t.height);
    }
}

// Colour blind modes: Machado 2009 simulation at full severity, then Fidaner's daltonize, which moves the difference
// the eye loses into channels it still sees. Rows of protanopia, deuteranopia, tritanopia.
constexpr float kColourBlindSimulate[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
constexpr float kColourBlindShift[3][9] = {
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};

// mode 1 to 3: identity + shift * (identity - simulate)
std::array<float, 9> ColourBlindMatrix(uint32_t mode) {
    const float* sim = kColourBlindSimulate[mode - 1];
    const float* shift = kColourBlindShift[mode - 1];
    std::array<float, 9> m{};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float lost = 0;
            for (int k = 0; k < 3; k++) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
            m[r * 3 + c] = float(r == c) + lost;
        }
    }
    return m;
}

// the frame over its box in the window (FrameBox), bars around it
void D3D12Backend::BlitToSwap(ID3D12Resource* src, D3D12_RESOURCE_STATES& state, DXGI_FORMAT fmt, DXGI_FORMAT rtv,
                              D3D12_CPU_DESCRIPTOR_HANDLE target, UINT mapping, uint32_t w, uint32_t h) {
    ID3D12PipelineState* pso = EnsureResolvePipeline(rtv, false, true);
    if (!pso) return;
    D3D12_RESOURCE_STATES* src_state = &state;
    if (fcip::Get().smaa) {
        Transition(src, state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        if (ID3D12Resource* aa = Smaa(src, fmt, w, h)) {
            src = aa;
            src_state = &smaa_state_[2];
        }
        cl_->OMSetRenderTargets(1, &target, FALSE, nullptr);
    }
    fcip::Box box = fcip::FrameBox(float(swap_w_), float(swap_h_));
    uint32_t views = AllocViews(kPsTextures);
    NullViews(views + 1, kPsTextures - 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = fmt;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = mapping;
    v.Texture2D.MipLevels = 1;
    Transition(src, *src_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    dev_->CreateShaderResourceView(src, &v, ViewCpu(views));
    uint32_t mode = std::min(fcip::Get().colour_blind, 3u);
    float cb[20] = {0, 0, float(w), float(h), 1, 0, 0, mode ? 1.0f : 0.0f};
    if (mode) {
        std::array<float, 9> colour = ColourBlindMatrix(mode);
        for (int row = 0; row < 3; row++) std::memcpy(cb + 8 + row * 4, colour.data() + row * 3, 3 * sizeof(float));
    }
    D3D12_GPU_VIRTUAL_ADDRESS addr = UploadConstants(cb, sizeof(cb));
    D3D12_VIEWPORT vp{box.x, box.y, box.w, box.h, 0, 1};
    D3D12_RECT sc{0, 0, LONG(swap_w_), LONG(swap_h_)};
    frame_rect_[0] = uint32_t(box.x);
    frame_rect_[1] = uint32_t(box.y);
    frame_rect_[2] = uint32_t(box.w);
    frame_rect_[3] = uint32_t(box.h);
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootVsConsts, addr);
    cl_->SetGraphicsRootConstantBufferView(kRootPsConsts, addr);
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->SetGraphicsRootDescriptorTable(kRootPsSamplers, LinearSamplers());
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl_->DrawInstanced(4, 1, 0, 0);
    Unbind();
}

// src readable: SMAA's blend of it, null when it can't run
ID3D12Resource* D3D12Backend::Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups()) return nullptr;
    if (!EnsureSmaaTargets(fmt, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, fmt, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].Get();
}

bool D3D12Backend::EnsureSmaaLookups() {
    // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
    constexpr SmaaLookup area{DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea};
    constexpr SmaaLookup search{DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch};
    static_assert(sizeof(kSmaaArea) == area.w * area.h * area.bpp);
    static_assert(sizeof(kSmaaSearch) == search.w * search.h * search.bpp);
    if (smaa_lut_[1]) return true;
    if (!CreateLookupTexture(smaa_lut_[0], area) || !CreateLookupTexture(smaa_lut_[1], search)) {
        smaa_lut_[0].Reset();
        smaa_lut_[1].Reset();
        return false;
    }
    return true;
}

// a lookup texture filled from l's data, left readable
bool D3D12Backend::CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(l.fmt, l.w, l.h);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    uint32_t row = l.w * l.bpp;
    UploadRows(r.Get(), 0, std::vector<uint8_t>(l.data, l.data + size_t(row) * l.h), row);
    Transition(r.Get(), st, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return true;
}

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
constexpr DXGI_FORMAT kSmaaEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

bool D3D12Backend::EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    const DXGI_FORMAT f[kSmaaTargets] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < kSmaaTargets; i++) {
        if (!CreateSmaaTarget(i, f[i], w, h)) {
            for (auto& r : smaa_) {
                if (r) Release(std::move(r));
            }
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGR("D3D12: SMAA %ux%u", w, h);
    return true;
}

bool D3D12Backend::CreateSmaaTarget(int i, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[i]) Release(std::move(smaa_[i]));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(fmt, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    smaa_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, smaa_state_[i], nullptr,
                                               IID_PPV_ARGS(&smaa_[i]));
    if (FAILED(hr)) return false;
    dev_->CreateRenderTargetView(smaa_[i].Get(), nullptr, SmaaView(i));
    return true;
}

// edges, blend weights, then the blend of src into smaa_[2], left readable
bool D3D12Backend::DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl) {
    // edge detection discards where there is no edge, so the edges start clear each frame
    const float clear[4] = {};
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(SmaaView(0), clear, 0, nullptr);
    ID3D12Resource* const edges[3] = {src, nullptr, nullptr};
    const DXGI_FORMAT edges_fmt[3] = {fmt, fmt, fmt};
    if (!Pass(hlsl, "vs_edges", "smaa_edges", kSmaaEdgeFormat, SmaaView(0), w, h, edges, edges_fmt, p, -1)) {
        return false;
    }
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(SmaaView(1), clear, 0, nullptr);
    ID3D12Resource* const weights[3] = {smaa_[0].Get(), smaa_lut_[0].Get(), smaa_lut_[1].Get()};
    const DXGI_FORMAT weights_fmt[3] = {kSmaaEdgeFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM};
    if (!Pass(hlsl, "vs_weights", "smaa_weights", kSmaaWeightFormat, SmaaView(1), w, h, weights, weights_fmt, p, -1)) {
        return false;
    }
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* const blend[3] = {src, smaa_[1].Get(), nullptr};
    const DXGI_FORMAT blend_fmt[3] = {fmt, kSmaaWeightFormat, fmt};
    if (!Pass(hlsl, "vs_blend", "smaa_blend", fmt, SmaaView(2), w, h, blend, blend_fmt, p, -1)) return false;
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return true;
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

// HDR10: the frame and the panel into their own targets, then both into the back buffer, the panel at SDR white
void D3D12Backend::PresentHdr(const xenos::TextureFetch& t, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    D3D12_CPU_DESCRIPTOR_HANDLE frame = Rtv(kOutRtv + kOutFrame);
    D3D12_CPU_DESCRIPTOR_HANDLE panel = Rtv(kOutRtv + kOutPanel);
    float black[4] = {0, 0, 0, 1};
    float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < kOutTargets; i++) Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(frame, black, 0, nullptr);
    cl_->ClearRenderTargetView(panel, clear, 0, nullptr);
    cl_->OMSetRenderTargets(1, &frame, FALSE, nullptr);
    DrawFront(t, kOutFrameFormat, frame);
    if (ext_) ext_->Presenting(*this);
    cl_->OMSetRenderTargets(1, &panel, FALSE, nullptr);
    RenderUi();
    Begin();  // RenderUi left ImGui's heap and root signature bound
    for (int i = 0; i < kOutTargets; i++) {
        Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    ID3D12Resource* const src[3] = {out_[kOutFrame].Get(), out_[kOutPanel].Get(), nullptr};
    const DXGI_FORMAT fmt[3] = {kOutFrameFormat, kSwapFormat, kSwapFormat};
    float p[8] = {paper_nits_, peak_nits_, 0, 0, 0, 0, 0, 0};
    Pass(kHdrHlsl, "hdr10", kHdr10Format, bb, swap_w_, swap_h_, src, fmt, p, -1);
}

// the frame and the UI into the back buffer, then shown
void D3D12Backend::ShowFrame(const xenos::TextureFetch& t) {
    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Rtv(kRtvCount + bi);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    if (!hdr_out_) {
        cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        DrawFront(t, kSwapFormat, rh);
        if (ext_) ext_->Presenting(*this);
        RenderUi();
    } else if (EnsureOutTargets()) {
        PresentHdr(t, rh);
    }
    if (generation_swap_ && hudless_.used == presents_) DrawHudless(t);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
    Submit(false);
    bool vsync = app::Get().vsync;
    if (generation_swap_ && upscale_.configured != presents_) ConfigureGeneration(false);
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

void D3D12Backend::Present(const uint32_t front[6]) {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(front);
    if (cap_) {
        FinishCapture(t);
    } else if (g_capture.exchange(false)) {
        StartCapture();
    }
    EndStamps();  // diag
    UpdateHdrDisplay();
    if (GenerationWanted() != generation_wanted_) RecreateSwapChain();
    ResizeSwap();
    ShowFrame(t);

    g_perf.upload_bytes += CurrentFrame().used;
    presents_++;
    frame_++;
    HashAhead();
    frame_textures_.clear();
    Frame& next = CurrentFrame();
    WaitFence(next.fence);
    next.alloc->Reset();
    next.garbage.clear();
    next.used = 0;
    ReadStamps();  // diag
    if (texturedumper::Update()) ReleasePackTextures();
    upload_epoch_++;
    next.views = 0;
    cl_->Reset(next.alloc.Get(), nullptr);
    Begin();
    Stamp(kStampOther);  // diag
    if (uint32_t side = fcip::SunShadowSide(); side != shadow_side_) {
        last_shadow_side_ = shadow_side_;
        shadow_side_ = side;
        side_changed_ = presents_;
    }
    DropIdleShadowMaps();
    caster_draws_.clear();
    gi_.draws.clear();
    gi_.capture = false;
    gi_.mode = 0;
    stamp_records_ += casters_used_;  // diag
    casters_used_ = 0;
    caster_slots_ = 0;
    casters_built_ = 0;
    std::erase_if(ray_shadows_, [&](auto& entry) {
        if (frame_ - entry.second.frame <= kRayIdleFrames) return false;
        Release(std::move(entry.second.bottom));
        Release(std::move(entry.second.top));
        return true;
    });

    fps_frames_++;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - fps_start_).count();
    if (secs >= 0.5) {
        fps_ = fps_frames_ / secs;
        fps_frames_ = 0;
        fps_start_ = now;
        if (posted_shaders_ || posted_pipelines_ || skipped_draws_) {
            LOGR("async shaders: %u shaders and %u pipelines started, %u draws waited", posted_shaders_,
                 posted_pipelines_, skipped_draws_);
            posted_shaders_ = 0;
            posted_pipelines_ = 0;
            skipped_draws_ = 0;
        }
    }
    if (diag::Perf()) LogPerf();
    if (BaseScale() != scale_ || SceneScale() != scene_scale_) {
        Submit(true);
        DropTargets();
        scale_ = BaseScale();
        scene_scale_ = SceneScale();
        upscale_.reset = true;
        if (ext_) ext_->Reset(*this);
        LOGR("D3D12: render scale %.3f", scale_);
    }
    if (app::Get().msaa != msaa_set_) {
        Submit(true);
        DropTargets();
        msaa_set_ = app::Get().msaa;
        msaa_ = Samples(msaa_set_);
        if (ext_) ext_->Reset(*this);
    }
    if (ext_ && ext_->Settings()) {
        Submit(true);
        DropTargets();
        ext_->Reset(*this);
    }
}

// Extension passes

// the draw state set since is set again by the next draw
void D3D12Backend::Unbind() {
    std::fill(std::begin(const_gpu_), std::end(const_gpu_), 0);
    binds_ = {};
}

// a copy of the target to read while drawing to it; a multisampled one is resolved into it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (t.depth) return nullptr;
    bool ms = t.samples > 1;
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = t.res->GetDesc();
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        d.SampleDesc.Count = 1;
        if (ms) d.Format = t.fmt;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    auto read = ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto write = ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(t.res.Get(), t.state, read);
    Transition(t.fb.Get(), t.fb_state, write);
    if (ms)
        cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    else
        cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return t.fb.Get();
}

// a full screen pipeline of hlsl's vs and entry, kept by its source; null when it cannot be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const std::string& hlsl, const char* vs_entry, const char* entry,
                                                      DXGI_FORMAT rtv, float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t key = Hash64(entry, std::strlen(entry), Hash64(hlsl.data(), hlsl.size(), seed));
    key = Hash64(vs_entry, std::strlen(vs_entry), key);
    auto& pso = pass_pipelines_[key];
    if (pso) return pso.Get();
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    if (!CompileHlsl(std::string("#define ") + vs_entry + " main\n" + hlsl, L"vs_6_0", vs)) return nullptr;
    if (!CompileHlsl(std::string("#define ") + entry + " main\n" + hlsl, L"ps_6_0", ps)) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs.data(), vs.size()};
    d.PS = {ps.data(), ps.size()};
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (blend >= 0) {
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::ShaderView(ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = res ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(res, &v, ViewCpu(slot));
}

// one triangle over the viewport: p in b1, the views table from t0, s0 linear and s1 point, both clamped
// s0 linear, s1 point, both clamped
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::LinearSamplers() {
    D3D12_SAMPLER_DESC linear{};
    linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_SAMPLER_DESC point = linear;
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    return EnsureSamplers({linear, point});
}

void D3D12Backend::FullScreen(ID3D12PipelineState* pso, uint32_t views, const float p[8]) {
    D3D12_GPU_DESCRIPTOR_HANDLE samplers = LinearSamplers();
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootFlow, UploadConstants(p, 8 * sizeof(float)));
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->SetGraphicsRootDescriptorTable(kRootPsSamplers, samplers);
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
    Unbind();
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha,
                        uint32_t taps, float mul, DXGI_FORMAT rtv) {
    ID3D12PipelineState* pso = EnsurePassPipeline(kBlitHlsl, "vs", "blit", rtv, -1);
    if (!pso) return;
    uint32_t views = AllocViews(kPsTextures);
    ShaderView(src, fmt, views);
    NullViews(views + 1, kPsTextures - 1);
    float p[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], gamma ? 1.0f : 0.0f, alpha ? 1.0f : 0.0f, float(taps),
                  mul};
    FullScreen(pso, views, p);
}

bool D3D12Backend::Pass(const char* hlsl, const char* vs_entry, const char* entry, DXGI_FORMAT rtv,
                        D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w, uint32_t h, ID3D12Resource* const src[3],
                        const DXGI_FORMAT fmt[3], const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, vs_entry, entry, rtv, blend);
    if (!pso) return false;
    uint32_t views = AllocViews(kPsTextures);
    for (uint32_t i = 0; i < 3; i++) ShaderView(src[i], fmt[i], views + i);
    NullViews(views + 3, kPsTextures - 3);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
    }
    FullScreen(pso, views, p);
    return true;
}

// Capture

// F3: each pixel texture the draw reads, from a resolve or guest memory
void D3D12Backend::CapTextures(const State& s, const ShaderInfo& ps) {
    for (uint32_t i = 0; i < ps.textures.size(); i++) {
        uint32_t fetch[6];
        ReadFetch(s, ps.textures[i].fetch, fetch);
        xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
        bool resolve = FindResolved(t, ps.textures[i], ShaderStage::Pixel, i) != nullptr;
        std::fprintf(cap_, "     t%u %08X %ux%u format %u %s\n", i, t.base, t.width, t.height, uint32_t(t.format),
                     resolve ? "resolve" : "memory");
    }
}

void D3D12Backend::StartCapture() {
    // without XEX_CAPTURE_DIR, beside the exe
    std::filesystem::path root = diag::CaptureDir();
    if (root.empty()) root = app::ExeDir() / "captures";
    cap_dir_ = root / ("frame_" + std::to_string(presents_));
    std::error_code ec;
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "draws.txt").c_str(), L"w");
    cap_i_ = 0;
    cap_rt_ = nullptr;
    if (cap_) LOGR("capture: frame %llu into %ls", static_cast<unsigned long long>(presents_), cap_dir_.c_str());
}

// the frame ends: its last target and the picture presented saved
void D3D12Backend::FinishCapture(const xenos::TextureFetch& front) {
    CapStep(nullptr);
    auto resolved = resolved_.find(ResolvedKey(front.base, 0));
    if (resolved != resolved_.end()) {
        Resolved& r = resolved->second;
        uint32_t w = uint32_t(std::ceil(r.width * r.scale));
        uint32_t h = uint32_t(std::ceil(r.height * r.scale));
        CapSave(r.res.Get(), r.state, r.format, 1, false, w, h, 1, "present");
    }
    std::fclose(cap_);
    cap_ = nullptr;
    LOGR("capture: %u steps saved", cap_i_);
}

// the frame moves to another colour target: save the one it leaves
void D3D12Backend::CapStep(Target* rt) {
    if (rt == cap_rt_) return;
    if (cap_rt_) {
        char name[48];
        std::snprintf(name, sizeof(name), "%04u_%u_%ux%u", cap_i_, cap_rt_->surface.base, cap_rt_->width,
                      cap_rt_->height);
        CapSave(*cap_rt_, name, cap_rt_->range);
    }
    cap_rt_ = rt;
}

void D3D12Backend::CapSave(Target& t, const std::string& name, float mul) {
    DXGI_FORMAT view = t.depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : ReadFormat(t);
    CapSave(t.res.Get(), t.state, view, t.samples, t.depth, t.w, t.h, t.depth ? 1 : mul, name);
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[48];
    std::snprintf(name, sizeof(name), "%04u_%s_%u", cap_i_++, tag, t.surface.base);
    CapSave(t, name, mul);
}

// drawn into an 8 bit copy, read back and written as a BMP; waits for the GPU
void D3D12Backend::CapSave(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, DXGI_FORMAT view, uint32_t samples,
                           bool depth, uint32_t w, uint32_t h, float mul, const std::string& name, UINT mapping) {
    constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12PipelineState* pso = EnsureResolvePipeline(kBgra, samples > 1);
    if (!pso || !w || !h || free_rtv_.empty()) return;
    ComPtr<ID3D12Resource> copy;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = TextureDesc(kBgra, w, h);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_STATES copy_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, copy_state, nullptr, IID_PPV_ARGS(&copy))))
        return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(total);
    if (FAILED(dev_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback))))
        return;
    uint32_t rtv = free_rtv_.back();
    dev_->CreateRenderTargetView(copy.Get(), nullptr, Rtv(rtv));
    uint32_t views = AllocViews(kPsTextures);
    NullViews(views + 1, kPsTextures - 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = view;
    v.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = mapping;
    v.Texture2D.MipLevels = 1;
    Transition(res, state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    dev_->CreateShaderResourceView(res, &v, ViewCpu(views));
    float cb[8] = {0, 0, float(w), float(h), float(samples), depth ? 1.0f : 0.0f, mul, 0};
    D3D12_GPU_VIRTUAL_ADDRESS addr = UploadConstants(cb, sizeof(cb));
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Rtv(rtv);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &sc);
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(kRootVsConsts, addr);
    cl_->SetGraphicsRootConstantBufferView(kRootPsConsts, addr);
    cl_->SetGraphicsRootDescriptorTable(kRootPsViews, ViewGuest(views));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl_->DrawInstanced(4, 1, 0, 0);
    Transition(copy.Get(), copy_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{copy.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Submit(true);
    uint8_t* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&px[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    readback->Unmap(0, nullptr);
    WriteBmp(cap_dir_ / (name + ".bmp"), w, h, px);
}

// F3: a float texture's values that are not finite, counted into draws.txt; waits for the GPU
void D3D12Backend::CapFinite(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, const char* what) {
    D3D12_RESOURCE_DESC d = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    DXGI_FORMAT f = fp.Footprint.Format;
    bool half = f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16_FLOAT;
    bool single = f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_R32G32_FLOAT;
    if (!half && !single) {
        std::fprintf(cap_, "%s: format %d, not float\n", what, int(f));
        return;
    }
    ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(total);
    if (FAILED(dev_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback))))
        return;
    D3D12_RESOURCE_STATES was = state;
    Transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(res, state, was);
    Submit(true);
    uint8_t* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    uint32_t size = half ? 2 : 4;
    uint32_t pixel = uint32_t(row_bytes / fp.Footprint.Width);
    uint64_t bad = 0;
    uint32_t first_x = 0;
    uint32_t first_y = 0;
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t* row = p + size_t(y) * fp.Footprint.RowPitch;
        for (uint32_t i = 0; i < row_bytes; i += size) {
            uint32_t v = 0;
            std::memcpy(&v, row + i, size);
            bool finite = half ? (v & 0x7C00) != 0x7C00 : (v & 0x7F800000) != 0x7F800000;
            if (finite) continue;
            if (!bad) {
                first_x = i / pixel;
                first_y = y;
            }
            bad++;
        }
    }
    readback->Unmap(0, nullptr);
    std::fprintf(cap_, "%s: format %d, %llu values not finite, first at %u,%u\n", what, int(f),
                 static_cast<unsigned long long>(bad), first_x, first_y);
}

// Queries

void D3D12Backend::BeginQuery(uint32_t q) {
    q %= kQueryCount;
    if (!queries_[q].open) open_queries_++;
    queries_[q] = {0, true};
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
}

void D3D12Backend::EndQuery(uint32_t q) {
    q %= kQueryCount;
    if (!queries_[q].open) return;
    queries_[q].open = false;
    open_queries_--;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, q, 1, query_read_.Get(), q * 8ull);
    // resolved in the next submission
    queries_[q].fence = fence_value_ + 1;
}

bool D3D12Backend::QueryResult(uint32_t q, uint32_t& samples) {
    q %= kQueryCount;
    const Query& query = queries_[q];
    if (query.open || !query.fence) return false;
    // a result of the frame still being recorded: the GPU gets that much, the title polls again as on the console
    if (query.fence > fence_value_) Kick();
    if (fence_->GetCompletedValue() < query.fence) return false;
    D3D12_RANGE range{q * 8ull, q * 8ull + 8};
    uint64_t* p = nullptr;
    if (FAILED(query_read_->Map(0, &range, reinterpret_cast<void**>(&p)))) return false;
    samples = uint32_t(std::min<uint64_t>(p[q], UINT32_MAX));
    D3D12_RANGE none{0, 0};
    query_read_->Unmap(0, &none);
    return true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

void NoteResolve(uint32_t base) {
    std::lock_guard lock(g_cpu.lock);
    g_cpu.issued[base]++;
}

void NoteFrame() {
    std::lock_guard lock(g_cpu.lock);
    for (auto& [base, rb] : g_cpu.read) {
        auto issued = g_cpu.issued.find(base);
        uint64_t count = issued == g_cpu.issued.end() ? 0 : issued->second;
        rb.frame_before = rb.last_frame;
        rb.last_frame = count - rb.frame_start;
        rb.frame_start = count;
    }
}

bool IsResolve(const uint32_t fetch[6]) {
    uint32_t base = xenos::DecodeTextureFetch(fetch).base;
    std::lock_guard lock(g_cpu.lock);
    return g_cpu.issued.contains(base);
}

// as the console's lock: waits for the GPU to finish the last resolve there, then the CPU sees it in guest memory.
// An address resolved the same number of times every frame is read a frame late, the same resolve of the frame before,
// so the title records ahead of the GPU instead of in step.
bool ApplyReadBack(const uint32_t fetch[6]) {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(fetch);
    std::unique_lock lock(g_cpu.lock);
    auto issued = g_cpu.issued.find(t.base);
    if (issued == g_cpu.issued.end() || t.mip_min || !CpuReadable(t)) return true;
    uint64_t want = issued->second;
    // copied at every resolve from now on
    ReadBack& seen = g_cpu.read.try_emplace(t.base).first->second;
    uint64_t per_frame = seen.last_frame;
    bool steady = per_frame >= 1 && per_frame < kReadBacks && per_frame == seen.frame_before;
    bool in_frame = want > per_frame && want - seen.frame_start <= per_frame;
    if (steady && in_frame) want -= per_frame;
    g_cpu.ran.wait(lock, [&] { return g_cpu.done[t.base] >= want; });
    auto read = g_cpu.read.find(t.base);
    if (read == g_cpu.read.end()) return true;
    ReadBack& rb = read->second;
    auto slot = std::find(std::begin(rb.seq), std::end(rb.seq), want) - std::begin(rb.seq);
    if (slot == kReadBacks) return false;
    // a texture of another size or format at the address replaced the resolve
    bool same = rb.format == HostTextureFormat(t) && rb.width == t.width && rb.height == t.height;
    if (!same) return true;
    g_cpu.fence->SetEventOnCompletion(rb.fence[slot], nullptr);
    uint8_t* p = nullptr;
    if (FAILED(rb.buffer[slot]->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return true;
    WriteTexels(p, rb.row_pitch, rb.copy_width, rb.copy_height, t);
    D3D12_RANGE none{0, 0};
    rb.buffer[slot]->Unmap(0, &none);
    return true;
}

uint64_t QueueCopy(const uint32_t dst[6]) {
    xenos::TextureFetch t = xenos::DecodeTextureFetch(dst);
    std::lock_guard lock(g_cpu.lock);
    if (t.base && !t.mip_min) g_cpu.issued.erase(t.base);
    return ++g_cpu.queued;
}

void WaitCopy(uint64_t n) {
    std::unique_lock lock(g_cpu.lock);
    g_cpu.ran.wait(lock, [&] { return g_cpu.run >= n; });
}

}  // namespace gpu
