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
#include <vector>

#include "common/settings.h"
#include "core/log.h"
#include "d3d/fsr.h"
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
};

std::vector<ArchiveShader> g_shaders;
std::atomic<size_t> g_next{0};
std::atomic<uint32_t> g_running{0};
std::atomic<bool> g_loading{false};
std::chrono::steady_clock::time_point g_start;

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
        shaders.push_back(std::move(a));
    }
    return shaders;
}

// the variants a first draw asks for: a vertex shader linked straight through, a pixel shader with and without its
// alpha test
void CompileArchiveShader(const ArchiveShader& a, bool motion) {
    gpu::ShaderOptions opt;
    opt.motion = motion;
    std::vector<uint8_t> dxil;
    if (a.vertex) {
        gpu::ShaderInfo info;
        std::string hlsl = gpu::TranslateShader(gpu::ShaderStage::Vertex, a.code.data(), a.code.size(), opt, info);
        gpu::CompileHlsl(hlsl, L"vs_6_0", dxil);
        return;
    }
    opt.vpos_reg = a.vpos;
    for (bool alpha : {false, true}) {
        opt.alpha_test = alpha;
        gpu::ShaderInfo info;
        std::string hlsl = gpu::TranslateShader(gpu::ShaderStage::Pixel, a.code.data(), a.code.size(), opt, info);
        gpu::CompileHlsl(hlsl, L"ps_6_0", dxil);
    }
}

// the game's threads come first, except on a loading screen, where it only draws the screen
void CompileArchive() {
    SetThreadDescription(GetCurrentThread(), L"shader archive");
    bool loading = false;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (size_t i = g_next++; i < g_shaders.size(); i = g_next++) {
        if (loading != g_loading) {
            loading = g_loading;
            SetThreadPriority(GetCurrentThread(), loading ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
        }
        // motion vectors as Draw adds them: FSR or TAA on and FSR's DLLs loaded
        bool motion = fcip::Temporal() && fsr::Upscaling();
        CompileArchiveShader(g_shaders[i], false);
        if (motion) CompileArchiveShader(g_shaders[i], true);
    }
    if (--g_running) return;
    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_start).count();
    LOGR("d3d: %zu archive shaders ready in %.1f s", g_shaders.size(), seconds);
}

}  // namespace

void PrecompileShaderArchive() {
    std::vector<uint8_t> file;
    if (!kernel::ReadGameFile(kShaderArchive, file)) return;
    g_shaders = ReadArchive(file);
    g_start = std::chrono::steady_clock::now();
    uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    g_running = threads;
    for (uint32_t t = 0; t < threads; t++) std::thread(CompileArchive).detach();
}

bool ShaderArchiveBusy() {
    g_loading = true;
    return g_running > 0;
}

}  // namespace d3d
