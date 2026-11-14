// Original Xbox ocean look from the level's <Ocean> values, above the sea only
#include "game/ocean_reflection.h"

#include <algorithm>

#include "common/render_pass.h"
#include "common/settings.h"
#include "game/scene.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::scene;

namespace {

// Cry3DEngine: the <Ocean> element's values as 825E71A0 reads them, copied whole by 825CE090
constexpr uint32_t kEngineOcean = 0x1190;
constexpr uint32_t kOceanReflectAmount = 0x3C;
constexpr uint32_t kOceanGlowBoostLevel = 0x4C;
// device constant shadows, 16 bytes a register: GlobalVariables.fx's WaterLevel
constexpr uint32_t kDevVsConstants = 0x780;
constexpr uint32_t kVsWaterLevel = 129;

// camera world height from its row major view
float CameraHeight(uint32_t camera) {
    uint32_t v = camera + kCameraView;
    return -(GF(v + 48) * GF(v + 32) + GF(v + 52) * GF(v + 36) + GF(v + 56) * GF(v + 40));
}

}  // namespace

void fcip::OceanSceneDraw() {
    if (SubmittedPass() != kPassCamera) return;
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t camera = Camera();
    OceanLook look;
    bool on = Get().ocean_reflection == kOceanXbox && engine && camera;
    if (on) {
        float sea = GF(R32(SYM_Device_pointer) + kDevVsConstants + kVsWaterLevel * 16);
        on = CameraHeight(camera) > sea;
        look.amount = std::clamp(GF(engine + kEngineOcean + kOceanReflectAmount), 0.0f, 1.0f);
        look.pivot = std::clamp(GF(engine + kEngineOcean + kOceanGlowBoostLevel), 0.0f, 1.0f);
    }
    SetOceanLook(on, look);
}
