// NVIDIA DLSS through Streamline, whose DLLs the player puts beside the exe
#pragma once
#include <cstdint>

#include "d3d/motion.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace dlss {

// before the device: Streamline from beside the exe, only when NVIDIA signed it
void Load();
// the device made: what this GPU and driver support
void SetDevice(ID3D12Device* dev);
// before the swap chain: its factory wrapped, so Streamline sees every present
void UpgradeFactory(void** factory);
// before the device goes
void Shutdown();

bool SuperResolution();
bool FrameGeneration();

// render resolutions with ANTI ALIASING on DLAA: the percentages DLSS names a mode for
struct ModePoint {
    uint32_t percent;
    const char* name;
};
inline constexpr ModePoint kModes[] = {
    {33, "ULTRA PERFORMANCE"}, {50, "PERFORMANCE"}, {58, "BALANCED"}, {67, "QUALITY"}, {100, "DLAA"},
};
constexpr uint32_t kMinPercent = 33;
constexpr uint32_t kMaxPercent = 100;

// a resource as DLSS reads or writes it
struct Image {
    ID3D12Resource* res = nullptr;
    uint32_t state = 0;   // D3D12_RESOURCE_STATES
    uint32_t format = 0;  // the typed DXGI_FORMAT of a typeless resource, 0 for its own
    uint32_t w = 0;
    uint32_t h = 0;
};

// one upscale: the scene drawn at the render resolution into output's size
struct Frame {
    Image color;
    Image depth;   // the title's, reversed
    Image motion;  // last frame's uv minus this frame's, jitter left out
    Image output;  // unordered access
    gpu::MotionCamera camera;
    float clip_to_prev[16] = {};  // the title's clip space, row major for row vectors
    float jitter[2] = {};         // pixels, as the projection was offset
    bool reset = false;           // nothing carries over from the last frame
    uint32_t index = 0;           // counts frames
};

// on the render thread's list; false when DLSS did not run
bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f);

}  // namespace dlss
