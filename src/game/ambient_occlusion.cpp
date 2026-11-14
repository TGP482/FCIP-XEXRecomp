// Ambient occlusion over the world only. The renderer's pass submit draws one list, radix sorted on key bits 0 to 19:
// the layer in bits 16 to 19, then the pipeline in bits 11 to 15, or the depth in the layers drawn far to near (5, 6,
// 9, 10 and 13 by table 0x8204AEB8; the ocean is on 7 and 11). In the camera's pass the characters (skinned items and
// the characters' pipeline) move to the end of layer 4 and occlusion goes in before the first item past the world, so
// the characters, the blended layers and the ocean land on top. The fog is linear over view distance, from the
// renderer's constant block. RTAO traces the world's draws from the start of the camera's list to that point. Global
// illumination traces every world layer, the characters included, and is added after the list's last item wherever a
// world draw left its albedo.
#include <algorithm>
#include <bit>
#include <cmath>

#include "common/render_pass.h"
#include "common/settings.h"
#include "core/log.h"
#include "d3d/occlusion.h"
#include "game/motion_vectors.h"
#include "game/scene.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::scene;

namespace {

// renderer
constexpr uint32_t kRendererPipelines = 0x10;  // the pipeline registry
// shader constant block, as the pass submit fills it
constexpr uint32_t kBlockFogScale = 0x21D4;  // fog amount / (end - start)
constexpr uint32_t kBlockFogBias = 0x21D8;   // -scale * start
constexpr uint32_t kBlockFogColour = 0x21E4;  // r, g, b from 0 to 1
// pipeline registry 82503538: entries of 0x18 bytes from +8 to +0xC, the name second
constexpr uint32_t kRegistryFirst = 0x8;
constexpr uint32_t kRegistryEnd = 0xC;
constexpr uint32_t kEntrySize = 0x18;
constexpr uint32_t kEntryName = 0x4;
constexpr uint32_t kEntryPipeline = 0x8;
constexpr uint32_t kCharactersName = 0x82070D10;  // "PerPixel Phong Color (Characters)"
// pipeline: vtable first, the item draw at +0xC; the PerPixel Phong draws all end in 8250DF28
constexpr uint32_t kPipelineDraw = 0xC;
constexpr uint32_t kPhongDraws[] = {0x8250DF28, 0x82504FB8, 0x825050F0, 0x825052B0};
// render item: its mesh, which 8250DF28 draws SKINNED when the byte at +0x40 is set
constexpr uint32_t kItemMesh = 0x18;
constexpr uint32_t kMeshSkinned = 0x40;
// Cry3DEngine: the flat ambient its objects take, as 825D1AB0 builds it less the sun it adds for particles
constexpr uint32_t kEngineVisAreas = 0x37C;  // VisArea_Find's first argument
constexpr uint32_t kEngineOutdoorAmbient = 0xB50;
constexpr uint32_t kEngineAmbientScale = 0x11FC;
constexpr uint32_t kEngineAmbientTint = 0x1200;
constexpr uint32_t kEngineFade = 0x1424;  // its scale at +0x440
constexpr uint32_t kFadeScale = 0x440;
constexpr uint32_t kVisAreaAmbient = 0xDC;
constexpr uint32_t kAmbientFloor = 0xC;  // past the outdoor addition at Ambient_pointer
// keys
constexpr uint32_t kPipelineShift = 11;
constexpr uint32_t kLastPipeline = 0x1F;
constexpr uint32_t kLayerAndPipeline = 0xFF800;  // key bits 11 to 19
constexpr uint32_t kMovedKey = kLastWorldLayer << kLayerShift | kLastPipeline << kPipelineShift;

// the registry's pipelines whose items may be characters
struct Pipelines {
    uint32_t characters = kLastPipeline;
    uint32_t phong = 0;  // a bit per pipeline
};

thread_local uint32_t t_sort_list = 0;  // the camera pass's list while its keys go in
thread_local Pipelines t_pipelines;
thread_local uint32_t t_draw_list = 0;  // the camera pass's list while it is drawn, until occlusion went in
thread_local uint32_t t_world_list = 0;  // the same while its world layers stream into global illumination's scene

bool Enabled() { return fcip::Get().ambient_occlusion && fcip::SubmittedPass() == fcip::kPassCamera; }
bool Illuminated() { return fcip::Get().global_illumination && fcip::SubmittedPass() == fcip::kPassCamera; }

bool PhongDraw(uint32_t draw) {
    for (uint32_t d : kPhongDraws) {
        if (draw == d) return true;
    }
    return false;
}

// the characters' pipeline and the PerPixel Phong ones among the first kLastPipeline
Pipelines FindPipelines() {
    Pipelines p;
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t registry = renderer ? R32(renderer + kRendererPipelines) : 0;
    if (!registry) return p;
    uint32_t first = R32(registry + kRegistryFirst);
    uint32_t end = R32(registry + kRegistryEnd);
    for (uint32_t i = 0; i < kLastPipeline && first + i * kEntrySize < end; i++) {
        uint32_t entry = first + i * kEntrySize;
        if (R32(entry + kEntryName) == kCharactersName) p.characters = i;
        uint32_t pipeline = R32(entry + kEntryPipeline);
        uint32_t vtable = pipeline ? R32(pipeline) : 0;
        if (vtable && PhongDraw(R32(vtable + kPipelineDraw))) p.phong |= 1u << i;
    }
    return p;
}

// the item drawn with this pipeline is a character
bool Character(uint32_t pipeline, uint32_t item) {
    if (pipeline == t_pipelines.characters) return true;
    if (!(t_pipelines.phong >> pipeline & 1)) return false;
    uint32_t mesh = R32(item + kItemMesh);
    return mesh && R8(mesh + kMeshSkinned);
}

// the item about to be popped lies past the world; the moved characters count as past it unless asked for
bool PastWorld(uint32_t list, bool characters) {
    uint32_t key = 0;
    if (!NextKey(list, key)) return false;
    uint32_t layer = key >> kLayerShift & kLayerMask;
    return layer > kLastWorldLayer || (!characters && (key & kLayerAndPipeline) == kMovedKey);
}

// a fog colour channel from 0 to 1 as a byte
uint32_t ColourByte(uint32_t addr) {
    float c = std::fmin(std::fmax(GF(addr), 0.0f), 1.0f);
    return uint32_t(std::lround(c * 255));
}

// the camera's projection and fog; false without a perspective camera
bool SceneArgs(gpu::OcclusionArgs& a) {
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t camera = Camera();
    uint32_t block = renderer ? R32(renderer + kRendererBlock) : 0;
    if (!camera || !block) return false;
    uint32_t m = camera + kCameraProjection;
    float x_scale = GF(m);
    float y_scale = GF(m + 5 * 4);
    bool perspective = GF(m + 11 * 4) == -1.0f && GF(m + 15 * 4) == 0.0f;
    if (!perspective || x_scale == 0 || y_scale == 0) return false;
    a.proj[0] = 1 / x_scale;
    a.proj[1] = 1 / y_scale;
    a.proj[2] = -GF(m + 8 * 4);
    a.proj[3] = -GF(m + 9 * 4);
    // the pass draws with depth flipped, z = w - z (823B0580)
    a.m22 = GF(m + 11 * 4) - GF(m + 10 * 4);
    a.m32 = GF(m + 15 * 4) - GF(m + 14 * 4);
    a.fog_scale = GF(block + kBlockFogScale);
    a.fog_bias = GF(block + kBlockFogBias);
    uint32_t r = ColourByte(block + kBlockFogColour);
    uint32_t g = ColourByte(block + kBlockFogColour + 4);
    uint32_t b = ColourByte(block + kBlockFogColour + 8);
    a.fog_colour = r << 16 | g << 8 | b;
    return true;
}

void Occlude() {
    t_draw_list = 0;
    gpu::OcclusionArgs a;
    if (!SceneArgs(a)) return;
    a.method = fcip::Get().ambient_occlusion;
    d3d::Occlusion(R32(SYM_Device_pointer), a);
}

// the ambient at the camera, of the vis area holding it or outdoors
bool Ambient(PPCContext& ctx, float out[3]) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t camera = Camera();
    uint32_t outdoor = R32(SYM_Ambient_pointer);
    if (!engine || !camera || !outdoor) return false;
    static const uint32_t pos = mem::AllocSystem(16);
    // the view is row major: the camera sits at minus its translation through the rotation's transpose
    uint32_t v = camera + kCameraView;
    for (uint32_t i = 0; i < 3; i++) {
        float t = GF(v + 48) * GF(v + i * 16) + GF(v + 52) * GF(v + i * 16 + 4) + GF(v + 56) * GF(v + i * 16 + 8);
        W32(pos + i * 4, std::bit_cast<uint32_t>(-t));
    }
    // the front end's scenes have no vis areas: VisArea_Find reads its manager unchecked
    uint32_t areas = R32(engine + kEngineVisAreas);
    uint32_t area = areas ? uint32_t(CallGuest(ctx, SYM_VisArea_Find, {areas, pos})) : 0;
    float scale = GF(engine + kEngineAmbientScale);
    uint32_t fade = R32(engine + kEngineFade);
    if (fade) scale *= GF(fade + kFadeScale);
    for (uint32_t i = 0; i < 3; i++) {
        float base = GF(outdoor + i * 4) + GF(engine + kEngineOutdoorAmbient + i * 4);
        if (area) base = GF(area + kVisAreaAmbient + i * 4);
        float floor = GF(outdoor + kAmbientFloor + i * 4);
        out[i] = std::fmax(base * scale * GF(engine + kEngineAmbientTint + i * 4), floor);
    }
    static float logged[3] = {-1, -1, -1};
    if (std::fabs(out[0] - logged[0]) + std::fabs(out[1] - logged[1]) + std::fabs(out[2] - logged[2]) > 0.02f) {
        LOGR("global illumination: ambient %.3f %.3f %.3f, %s", out[0], out[1], out[2], area ? "vis area" : "outdoors");
        std::copy(out, out + 3, logged);
    }
    return true;
}

// the world ends where the first item past it is popped, else with the list; the ambient goes with it
void EndWorld(PPCContext& ctx) {
    t_world_list = 0;
    float ambient[3] = {};
    Ambient(ctx, ambient);
    d3d::IlluminationWorld(R32(SYM_Device_pointer), ambient);
}

void Illuminate(PPCContext& ctx) {
    if (t_world_list) EndWorld(ctx);
    gpu::OcclusionArgs a;
    if (SceneArgs(a)) d3d::Illumination(R32(SYM_Device_pointer), a);
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Sort));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Push));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Draw));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Pop));

// the list's keys made and pushed, r3 the constant block
PPC_FUNC(GUEST(Scene_Sort)) {
    if (Enabled()) {
        t_sort_list = uint32_t(ctx.r[3]) + kBlockList;
        t_pipelines = FindPipelines();
    }
    GUEST_ORIG(Scene_Sort)(ctx, base);
    t_sort_list = 0;
}

// (list, key, item): a character in the world layers sorts after every other world item
PPC_FUNC(GUEST(Scene_Push)) {
    uint32_t key = uint32_t(ctx.r[4]);
    bool world = (key >> kLayerShift & kLayerMask) <= kLastWorldLayer;
    uint32_t pipeline = key >> kPipelineShift & kLastPipeline;
    bool sorting = t_sort_list == uint32_t(ctx.r[3]);
    if (sorting && world && Character(pipeline, uint32_t(ctx.r[5]))) ctx.r[4] = (key & ~kLayerAndPipeline) | kMovedKey;
    GUEST_ORIG(Scene_Push)(ctx, base);
}

// the list drawn in key order, r3 the constant block; occlusion goes in at the end when nothing past the world came
PPC_FUNC(GUEST(Scene_Draw)) {
    fcip::MotionSceneDraw(uint32_t(ctx.r[3]));
    if (Enabled()) {
        t_draw_list = uint32_t(ctx.r[3]) + kBlockList;
        if (fcip::Get().ambient_occlusion == gpu::kAoRtao) d3d::OcclusionScene(R32(SYM_Device_pointer));
    }
    bool lit = Illuminated();
    if (lit) {
        t_world_list = uint32_t(ctx.r[3]) + kBlockList;
        d3d::IlluminationScene(R32(SYM_Device_pointer));
    }
    GUEST_ORIG(Scene_Draw)(ctx, base);
    if (t_draw_list) Occlude();
    if (lit) Illuminate(ctx);
    fcip::MotionSceneDone();
}

// the next item of a list, r3 the list
PPC_FUNC(GUEST(Scene_Pop)) {
    uint32_t list = uint32_t(ctx.r[3]);
    if (t_draw_list && t_draw_list == list && PastWorld(list, false)) Occlude();
    if (t_world_list && t_world_list == list && PastWorld(list, true)) EndWorld(ctx);
    fcip::MotionScenePop(list);
    GUEST_ORIG(Scene_Pop)(ctx, base);
}
