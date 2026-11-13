// The camera's pass marks its world for the renderer's motion vectors: from the first item to the first past the world
// layers, the characters at the end of layer 4 included, handing over the camera's matrices first
#include "game/motion_vectors.h"

#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/motion.h"
#include "game/scene.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::scene;

namespace {

thread_local uint32_t t_list = 0;  // the camera pass's list from its first item to the first past the world

void ReadMatrix(uint32_t addr, float out[16]) {
    for (uint32_t i = 0; i < 16; i++) out[i] = GF(addr + i * 4);
}

void End() {
    t_list = 0;
    d3d::MotionEnd(R32(SYM_Device_pointer));
}

}  // namespace

namespace fcip {

void MotionSceneDraw(uint32_t block) {
    if (!Get().dlaa || SubmittedPass() != kPassCamera) return;
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
    if ((key >> kLayerShift & kLayerMask) > kLastWorldLayer) End();
}

void MotionSceneDone() {
    if (t_list) End();
}

}  // namespace fcip
