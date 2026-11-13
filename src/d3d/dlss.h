// NVIDIA DLSS through Streamline, whose DLLs the player puts in the DLSS folder beside the exe
#pragma once
#include <cstdint>

#include "d3d/motion.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace dlss {

// before the device: Streamline from the DLSS folder, only when NVIDIA signed it
void Load();
// the device made: what this GPU and driver support
void SetDevice(ID3D12Device* dev);
// before the swap chain: its factory wrapped, so Streamline sees every present
void UpgradeFactory(void** factory);
// before the device goes
void Shutdown();

bool SuperResolution();
bool FrameGeneration();

// why a DLSS feature does not run: its DLLs are not in the DLSS folder, or this GPU or driver lacks it
enum class Status { Ready, MissingDll, NotSupported };
Status UpscalingStatus();
Status GenerationStatus();
// the menu's note after a value, empty when ready
const char* StatusNote(Status s);
uint32_t MaxMultiplier();  // frames presented per rendered one, 1 without frame generation
bool DynamicFrameGeneration();

// the FRAME GENERATION setting: off, dynamic, else the multiplier
constexpr uint32_t kGenerationOff = 0;
constexpr uint32_t kGenerationDynamic = 1;

// Reflex and frame generation's markers, by frame
enum class Marker : uint32_t {
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd,
};
void Mark(Marker m, uint32_t frame);
// title thread, before its simulation starts
void Sleep(uint32_t frame);
// render thread, before the swap chain presents frame: the setting, whether the frame is gameplay DLSS drew, and the
// frame's part of the back buffer
void Generate(uint32_t frame, uint32_t setting, bool gameplay, const uint32_t rect[4], uint32_t w, uint32_t h);

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
