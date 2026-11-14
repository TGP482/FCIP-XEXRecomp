// The camera's pass marks its motion vectors for the renderer, from its list to the scene's end, the first person weapon
// in its late layers included, handing over the camera's matrices first. The ocean's layers are left out: its projected
// grid follows the camera, and its hundreds of copies of one mesh are costly to match
#include "game/motion_vectors.h"

#include <utility>

#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/motion.h"
#include "game/scene.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::scene;

namespace {

constexpr uint32_t kOceanLayers = 1u << 7 | 1u << 11;

thread_local uint32_t t_list = 0;  // the camera pass's list
thread_local bool t_paused = false;

void ReadMatrix(uint32_t addr, float out[16]) {
    for (uint32_t i = 0; i < 16; i++) out[i] = GF(addr + i * 4);
}

}  // namespace

namespace fcip {

void MotionSceneDraw(uint32_t block) {
    if (!Get().fsr || SubmittedPass() != kPassCamera) return;
    uint32_t camera = Camera();
    if (!camera) return;
    gpu::MotionCamera c;
    ReadMatrix(camera + kCameraView, c.view);
    ReadMatrix(camera + kCameraProjection, c.proj);
    d3d::MotionBegin(R32(SYM_Device_pointer), c);
    t_list = block + kBlockList;
}

void MotionScenePop(uint32_t list) {
    uint32_t key = 0;
    if (!t_list || t_list != list || !NextKey(list, key)) return;
    bool ocean = kOceanLayers >> (key >> kLayerShift & kLayerMask) & 1;
    if (ocean == t_paused) return;
    t_paused = ocean;
    if (ocean) d3d::MotionPause(R32(SYM_Device_pointer));
    else d3d::MotionResume(R32(SYM_Device_pointer));
}

void MotionSceneDone() {
    t_list = 0;
    if (std::exchange(t_paused, false)) d3d::MotionResume(R32(SYM_Device_pointer));
}

}  // namespace fcip
