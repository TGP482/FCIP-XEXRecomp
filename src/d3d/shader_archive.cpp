#include "d3d/shader_archive.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/render_pass.h"
#include "common/settings.h"
#include "core/log.h"
#include "d3d/fsr.h"
#include "d3d/occlusion.h"
#include "gpu/compiler.h"
#include "gpu/shaders.h"
#include "kernel/kernel.h"
#include "xdk/layout.h"

namespace d3d {

namespace {

constexpr const char* kShaderArchive = "game:\\shaders.dat";

struct ArchiveShader {
    bool vertex = false;
    int8_t vpos = -1;  // pixel: the register given VPOS
    std::vector<uint32_t> code;
    uint64_t hash = 0;  // as the renderer's Shader::hash
};

// the variants the settings draw with first and the recorded pipelines, which loading screens wait for; then, with
// FSR's DLLs loaded, the motion vector variants the settings leave out, so turning FSR or TAA on later finds them built
std::vector<ArchiveShader> g_shaders;
std::unordered_map<uint64_t, size_t> g_by_hash;
size_t g_listed = 0;
bool g_motion_first = false;
size_t g_jobs = 0;
std::atomic<size_t> g_next{0};
std::atomic<size_t> g_done{0};  // of the first pass
std::atomic<size_t> g_motion_done{0};
std::atomic<bool> g_loading{false};
std::chrono::steady_clock::time_point g_start;

size_t FirstPassJobs() { return g_shaders.size() + g_listed; }

bool FirstPassDone() { return g_done >= FirstPassJobs(); }

void LogReady(const char* what) {
    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_start).count();
    LOGR("d3d: %zu archive shaders and %zu recorded pipelines ready%s in %.1f s", g_shaders.size(), g_listed, what,
         seconds);
}

uint32_t Be32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, &b[at], 4);
    return _byteswap_ulong(v);
}

// the archive's containers, found by their magic: low byte 0 pixel, 1 vertex
std::vector<ArchiveShader> ReadArchive(const std::vector<uint8_t>& file) {
    constexpr size_t kMaxBytes = 0x100000;
    constexpr size_t kHeaderBytes = 0x18;
    std::vector<ArchiveShader> shaders;
    for (size_t c = 0; c + kHeaderBytes <= file.size(); c += 4) {
        uint32_t magic = Be32(file, c);
        if (magic >> 8 != XDK_Shader_Magic || (magic & 0xFF) > 1) continue;
        size_t virtual_size = Be32(file, c + 4);
        size_t info = c + Be32(file, c + XDK_Shader_Info);
        if (virtual_size > kMaxBytes || info + 8 > file.size()) continue;
        size_t code_at = c + virtual_size;
        size_t code_bytes = 0;
        if constexpr (XDK_ShaderInfo_Code != xdk::kUnknown) {
            code_at += Be32(file, info + XDK_ShaderInfo_Code);
            code_bytes = Be32(file, info + XDK_ShaderInfo_CodeSize);
        } else {
            code_bytes = Be32(file, c + XDK_Shader_CodeSize);
        }
        if (!code_bytes || code_bytes > kMaxBytes || code_at + code_bytes > file.size()) continue;
        ArchiveShader a;
        a.vertex = magic & 1;
        // SQ_PROGRAM_CNTL param_gen, the register from SQ_CONTEXT_MISC
        if (!a.vertex && (Be32(file, info + XDK_ShaderInfo_ProgramControl) >> 18 & 1)) {
            a.vpos = int8_t(Be32(file, info + XDK_ShaderInfo_ContextMisc) >> 8 & 0xFF);
        }
        a.code.resize(code_bytes / 4);
        for (size_t i = 0; i < a.code.size(); i++) a.code[i] = Be32(file, code_at + i * 4);
        a.hash = gpu::Hash64(a.code.data(), a.code.size() * 4);
        shaders.push_back(std::move(a));
    }
    return shaders;
}

void Compile(const ArchiveShader& a, const gpu::ShaderOptions& opt, gpu::ShaderInfo& info) {
    gpu::ShaderStage stage = a.vertex ? gpu::ShaderStage::Vertex : gpu::ShaderStage::Pixel;
    const wchar_t* profile = nullptr;
    std::string hlsl = gpu::VariantHlsl(stage, a.code.data(), a.code.size(), opt, info, profile);
    std::vector<uint8_t> dxil;
    gpu::CompileHlsl(hlsl, profile, dxil);
}

// a light's shadow sampler, which draws give a kShadowOut mode whichever map they bind to it
bool ReadsShadows(const gpu::ShaderInfo& info) {
    if (info.textures.size() > fcip::kShadowOut) return false;
    auto shadow = [](const gpu::TextureBinding& b) {
        return b.dimension <= 1 && b.fetch >= fcip::kShadowFirstFetch && b.fetch <= fcip::kSunFetch;
    };
    return std::any_of(info.textures.begin(), info.textures.end(), shadow);
}

// the kShadowOut modes the settings draw shadows with: the sharper sun maps past 1x shadow distance, traced with ray
// traced shadows. Soft filters flag each map apart, so their variants are left to the draws
std::vector<uint8_t> ShadowModes() {
    const fcip::Settings& s = fcip::Get();
    bool soft = s.shadow_filter == fcip::kFilterSoft || s.shadow_filter == fcip::kFilterPcss;
    std::vector<uint8_t> modes;
    if (s.draw_distance > 1 && !soft) modes.push_back(fcip::kShadowCascades);
    if (s.shadow_filter >= fcip::kFilterRay && gpu::RayTracing()) modes.push_back(fcip::kShadowTraced);
    return modes;
}

// the variants a first draw asks for: a vertex shader linked straight through, a pixel shader with and without its
// alpha test, and for a shadow receiver its shadow modes, a traced alpha test with its depth pass
void CompileArchiveShader(const ArchiveShader& a, bool motion) {
    gpu::ShaderOptions opt;
    opt.motion = motion;
    gpu::ShaderInfo info;
    if (a.vertex) {
        Compile(a, opt, info);
        return;
    }
    opt.vpos_reg = a.vpos;
    bool ultra = fcip::Get().shadow_filter == fcip::kFilterRayUltra;
    for (bool alpha : {false, true}) {
        opt.alpha_test = alpha;
        Compile(a, opt, info);
        if (!ReadsShadows(info)) continue;
        for (uint8_t mode : ShadowModes()) {
            gpu::ShaderOptions shadow = opt;
            shadow.tex_flags[fcip::kShadowOut] = mode;
            bool traced = mode == fcip::kShadowTraced;
            if (traced && alpha && ultra) shadow.tex_flags[fcip::kRayCoarseOut] = 1;
            gpu::ShaderInfo shadow_info;
            Compile(a, shadow, shadow_info);
            if (!traced || !alpha) continue;
            gpu::ShaderOptions depth = shadow;
            depth.tex_flags[fcip::kShadowOut] = 0;
            depth.tex_flags[fcip::kDepthOut] = 1;
            Compile(a, depth, shadow_info);
        }
    }
}

// the game's threads come first, except on a loading screen holding for the first pass, where it only draws the screen
void CompileArchive() {
    SetThreadDescription(GetCurrentThread(), L"shader archive");
    bool hold = false;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    size_t count = g_shaders.size();
    size_t first = FirstPassJobs();
    for (size_t j = g_next++; j < g_jobs; j = g_next++) {
        if (hold != (g_loading && !FirstPassDone())) {
            hold = !hold;
            SetThreadPriority(GetCurrentThread(), hold ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
        }
        if (j >= first) {
            CompileArchiveShader(g_shaders[j - first], !g_motion_first);
            if (++g_motion_done == count) LogReady(" with motion vectors");
            continue;
        }
        if (j >= count) {
            gpu::BuildListedPipeline(j - count);
            if (++g_done == first) LogReady("");
            continue;
        }
        CompileArchiveShader(g_shaders[j], false);
        if (g_motion_first) CompileArchiveShader(g_shaders[j], true);
        if (++g_done == first) LogReady("");
    }
}

}  // namespace

void PrecompileShaderArchive() {
    std::vector<uint8_t> file;
    if (!kernel::ReadGameFile(kShaderArchive, file)) return;
    g_shaders = ReadArchive(file);
    for (size_t i = 0; i < g_shaders.size(); i++) g_by_hash.emplace(g_shaders[i].hash, i);
    g_listed = gpu::ListedPipelineCount();
    // motion vectors as Draw adds them: FSR, TAA or frame generation on and FSR's shaders present
    g_motion_first = fcip::Motion() && fsr::Upscaling();
    g_jobs = FirstPassJobs() + (fsr::Upscaling() && !g_motion_first ? g_shaders.size() : 0);
    g_start = std::chrono::steady_clock::now();
    uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    for (uint32_t t = 0; t < threads; t++) std::thread(CompileArchive).detach();
}

bool ShaderArchiveBusy() {
    g_loading = true;
    return !FirstPassDone();
}

int ShaderArchivePercent() {
    if (!g_loading || FirstPassDone()) return -1;
    return int(g_done * 100 / FirstPassJobs());
}

const std::vector<uint32_t>* ArchiveCode(uint64_t hash) {
    auto it = g_by_hash.find(hash);
    return it == g_by_hash.end() ? nullptr : &g_shaders[it->second].code;
}

}  // namespace d3d
