// Ambient occlusion over the world only. The renderer's pass submit draws one list, radix sorted on key bits 0 to 19:
// the layer in bits 16 to 19, then the pipeline in bits 11 to 15, or the depth in the layers drawn far to near (5, 6,
// 9, 10 and 13 by table 0x8204AEB8; the ocean is on 7 and 11). In the camera's pass the characters' pipeline moves to
// the end of layer 4 and occlusion goes in before the first item past the world, so the characters, the blended
// layers and the ocean land on top. The fog is linear over view distance, from the renderer's constant block.
#include <cmath>

#include "common/render_pass.h"
#include "common/settings.h"
#include "d3d/occlusion.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// renderer
constexpr uint32_t kRendererBlock = 0xC;       // the shader constant block, its draw list at +0x24
constexpr uint32_t kRendererPipelines = 0x10;  // the pipeline registry
constexpr uint32_t kRendererPass = 0x90;       // the pass being submitted, its camera first
// shader constant block, as the pass submit fills it
constexpr uint32_t kBlockList = 0x24;
constexpr uint32_t kBlockFogScale = 0x21D4;  // fog amount / (end - start)
constexpr uint32_t kBlockFogBias = 0x21D8;   // -scale * start
constexpr uint32_t kBlockFogColour = 0x21E4;  // r, g, b from 0 to 1
// pipeline registry 82503538: entries of 0x18 bytes from +8 to +0xC, the name second
constexpr uint32_t kRegistryFirst = 0x8;
constexpr uint32_t kRegistryEnd = 0xC;
constexpr uint32_t kEntrySize = 0x18;
constexpr uint32_t kEntryName = 0x4;
constexpr uint32_t kCharactersName = 0x82070D10;  // "PerPixel Phong Color (Characters)"
// camera: PerspectiveFov's right handed projection, row major, before the pass flips its depth
constexpr uint32_t kCameraProjection = 0x23C;
// draw list 825091A0: the count first, entries {key, item, next} of 0xC bytes at +8, the bucket being popped
constexpr uint32_t kListEntries = 0x8;
constexpr uint32_t kListCursor = 0x201C;
constexpr uint32_t kListEntrySize = 0xC;
// keys
constexpr uint32_t kLayerShift = 16;
constexpr uint32_t kLayerMask = 0xF;
constexpr uint32_t kPipelineShift = 11;
constexpr uint32_t kLastWorldLayer = 4;
constexpr uint32_t kLastPipeline = 0x1F;
constexpr uint32_t kLayerAndPipeline = 0xFF800;  // key bits 11 to 19
constexpr uint32_t kMovedKey = kLastWorldLayer << kLayerShift | kLastPipeline << kPipelineShift;
constexpr uint32_t kGtao = 2;  // the setting, 1 SSAO

thread_local uint32_t t_sort_list = 0;  // the camera pass's list while its keys go in
thread_local uint32_t t_characters = kLastPipeline;
thread_local uint32_t t_draw_list = 0;  // the camera pass's list while it is drawn, until occlusion went in

bool Enabled() { return fcip::Get().ambient_occlusion && fcip::SubmittedPass() == fcip::kPassCamera; }

// the characters' pipeline, kLastPipeline when none has a number below it
uint32_t CharactersPipeline() {
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t registry = renderer ? R32(renderer + kRendererPipelines) : 0;
    if (!registry) return kLastPipeline;
    uint32_t first = R32(registry + kRegistryFirst);
    uint32_t end = R32(registry + kRegistryEnd);
    for (uint32_t i = 0; i < kLastPipeline && first + i * kEntrySize < end; i++) {
        if (R32(first + i * kEntrySize + kEntryName) == kCharactersName) return i;
    }
    return kLastPipeline;
}

// the item about to be popped lies past the world
bool PastWorld(uint32_t list) {
    uint32_t cursor = R32(list + kListCursor);
    if (!R32(list) || !cursor) return false;
    int32_t index = int32_t(R32(cursor));
    if (index < 0) return false;
    uint32_t key = R32(R32(list + kListEntries) + uint32_t(index) * kListEntrySize);
    uint32_t layer = key >> kLayerShift & kLayerMask;
    return layer > kLastWorldLayer || (key & kLayerAndPipeline) == kMovedKey;
}

// a fog colour channel from 0 to 1 as a byte
uint32_t ColourByte(uint32_t addr) {
    float c = std::fmin(std::fmax(GF(addr), 0.0f), 1.0f);
    return uint32_t(std::lround(c * 255));
}

void Occlude() {
    t_draw_list = 0;
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t pass = renderer ? R32(renderer + kRendererPass) : 0;
    uint32_t camera = pass ? R32(pass) : 0;
    uint32_t block = renderer ? R32(renderer + kRendererBlock) : 0;
    if (!camera || !block) return;
    uint32_t m = camera + kCameraProjection;
    float x_scale = GF(m);
    float y_scale = GF(m + 5 * 4);
    bool perspective = GF(m + 11 * 4) == -1.0f && GF(m + 15 * 4) == 0.0f;
    if (!perspective || x_scale == 0 || y_scale == 0) return;
    gpu::OcclusionArgs a;
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
    a.gtao = fcip::Get().ambient_occlusion == kGtao;
    d3d::Occlusion(R32(SYM_Device_pointer), a);
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
        t_characters = CharactersPipeline();
    }
    GUEST_ORIG(Scene_Sort)(ctx, base);
    t_sort_list = 0;
}

// (list, key, item): a character in the world layers sorts after every other world item
PPC_FUNC(GUEST(Scene_Push)) {
    uint32_t key = uint32_t(ctx.r[4]);
    bool world = (key >> kLayerShift & kLayerMask) <= kLastWorldLayer;
    bool character = t_characters < kLastPipeline && (key >> kPipelineShift & kLastPipeline) == t_characters;
    if (t_sort_list == uint32_t(ctx.r[3]) && world && character) ctx.r[4] = (key & ~kLayerAndPipeline) | kMovedKey;
    GUEST_ORIG(Scene_Push)(ctx, base);
}

// the list drawn in key order, r3 the constant block; occlusion goes in at the end when nothing past the world came
PPC_FUNC(GUEST(Scene_Draw)) {
    if (Enabled()) t_draw_list = uint32_t(ctx.r[3]) + kBlockList;
    GUEST_ORIG(Scene_Draw)(ctx, base);
    if (t_draw_list) Occlude();
}

// the next item of a list, r3 the list
PPC_FUNC(GUEST(Scene_Pop)) {
    uint32_t list = uint32_t(ctx.r[3]);
    if (t_draw_list && t_draw_list == list && PastWorld(list)) Occlude();
    GUEST_ORIG(Scene_Pop)(ctx, base);
}
