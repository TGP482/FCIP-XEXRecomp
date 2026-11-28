#pragma once
#include <cstdint>

namespace gpu::prof {

enum Bucket : uint8_t {
    kTotal = 0,
    kShadowRaster,
    kRtBuild,
    kZPrepass,
    kSunMask,
    kForwardTraced,
    kForwardOther,
    kAo,
    kGi,
    kFog,
    kClouds,
    kCaustics,
    kUpscale,
    kResolve,
    kPost,
    kBucketCount,
};

inline constexpr const char* kBucketNames[kBucketCount] = {
    "Total GPU", "Shadow Raster", "RT AS Build", "World Z-Prepass", "Sun RT Mask", "Forward (RT)",
    "Forward (Other)", "AO", "RTGI", "Vol. Fog", "Vol. Clouds", "Caustics", "Motion/TAA/FSR", "Resolves",
    "SMAA/Present",
};

struct Snapshot {
    float ms[kBucketCount] = {};
    float cpu_ms = 0;
    float fps = 0;
    uint32_t draws = 0;
    uint32_t rt_draws = 0;
    uint32_t blas_builds = 0;
    uint32_t caster_tris = 0;
    uint32_t resolves = 0;
    float upload_mb = 0;
    bool valid = false;
};

bool Enabled();
void Toggle();
Snapshot Latest();

}  // namespace gpu::prof
