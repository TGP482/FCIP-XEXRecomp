// The EDRAM restore after the glow (823AE398) copies its texture back with a quad from the viewport's corners whose
// texture coordinates start half a texel in: upscaled, the half pixel shift then leaves the top row and left column
// of host pixels black, which the radial blur smears into bars. Moved half a pixel up and left, coordinates with it,
// it covers every host pixel and samples the same texels.
#include <bit>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// 823AE398: the copy's viewport size in its globals (r31), the texture's in its level description (r1)
constexpr uint32_t kRestoreWidth = 0x198;
constexpr uint32_t kRestoreHeight = 0x19C;
constexpr uint32_t kTextureWidth = 0xA8;
constexpr uint32_t kTextureHeight = 0xAC;
// vertices of x, y in clip space then u, v
constexpr uint32_t kVertices = 4;
constexpr uint32_t kVertexSize = 0x18;
constexpr uint32_t kVertexU = 0x8;

void AddFloat(uint32_t at, float add) { W32(at, std::bit_cast<uint32_t>(std::bit_cast<float>(R32(at)) + add)); }

}  // namespace

// before the restore's draw: r6 its vertices, r23 set when it copies a texture
void RestoreQuad(PPCContext& ctx, uint8_t*) {
    if (!uint8_t(ctx.r[23])) return;
    uint32_t globals = uint32_t(ctx.r[31]);
    uint32_t stack = uint32_t(ctx.r[1]);
    float w = float(R32(globals + kRestoreWidth));
    float h = float(R32(globals + kRestoreHeight));
    float tw = float(R32(stack + kTextureWidth));
    float th = float(R32(stack + kTextureHeight));
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0) return;
    for (uint32_t i = 0; i < kVertices; i++) {
        uint32_t v = uint32_t(ctx.r[6]) + i * kVertexSize;
        // clip space y points up
        AddFloat(v, -1 / w);
        AddFloat(v + 4, 1 / h);
        AddFloat(v + kVertexU, -0.5f / tw);
        AddFloat(v + kVertexU + 4, -0.5f / th);
    }
}
