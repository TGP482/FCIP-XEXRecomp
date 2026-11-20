// The EDRAM restore after the glow (default.xex 823AE398, ige.xex 82372C08) copies its textures back with a quad from
// the viewport's corners whose texture coordinates start half a texel in: upscaled, the half pixel shift then leaves
// the top row and left column of host pixels black, which the radial blur smears into bars. Moved half a pixel up and
// left, coordinates with it, it covers every host pixel and samples the same texels.
#include <bit>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// the display block: the restore's viewport
constexpr uint32_t kBlockViewWidth = 0x198;
constexpr uint32_t kBlockViewHeight = 0x19C;
// a texture's level description, as the restore reads it onto its stack
constexpr uint32_t kDescWidth = 0x18;
constexpr uint32_t kDescHeight = 0x1C;
// vertices of x, y in clip space then each texture's u, v
constexpr uint32_t kVertices = 4;
constexpr uint32_t kVertexSize = 0x18;
constexpr uint32_t kVertexCoords = 0x8;
// default.xex: one texture, its description at r1 + 0x90
constexpr uint32_t kGameDesc = 0x90;
// ige.xex: a second texture past the first, descriptions at r1 + 0xF0 and r1 + 0x110
constexpr uint32_t kEditorDescs[] = {0xF0, 0x110};

void AddFloat(uint32_t at, float add) { W32(at, std::bit_cast<uint32_t>(std::bit_cast<float>(R32(at)) + add)); }

void ShiftCorners(uint32_t vertices) {
    float w = float(R32(SYM_DisplayBlock + kBlockViewWidth));
    float h = float(R32(SYM_DisplayBlock + kBlockViewHeight));
    if (w <= 0 || h <= 0) return;
    for (uint32_t i = 0; i < kVertices; i++) {
        uint32_t v = vertices + i * kVertexSize;
        // clip space y points up
        AddFloat(v, -1 / w);
        AddFloat(v + 4, 1 / h);
    }
}

// texture n's coordinates, its description at desc
void ShiftCoords(uint32_t vertices, uint32_t n, uint32_t desc) {
    float w = float(R32(desc + kDescWidth));
    float h = float(R32(desc + kDescHeight));
    if (w <= 0 || h <= 0) return;
    for (uint32_t i = 0; i < kVertices; i++) {
        uint32_t uv = vertices + i * kVertexSize + kVertexCoords + n * 8;
        AddFloat(uv, -0.5f / w);
        AddFloat(uv + 4, -0.5f / h);
    }
}

}  // namespace

// default.xex, before the restore's draw: r6 its vertices, r23 set when it copies a texture
void RestoreQuad(PPCContext& ctx, uint8_t*) {
    if (!uint8_t(ctx.r[23])) return;
    ShiftCorners(uint32_t(ctx.r[6]));
    ShiftCoords(uint32_t(ctx.r[6]), 0, uint32_t(ctx.r[1]) + kGameDesc);
}

// ige.xex, the same: r29 set when it copies a texture, r22 when a second one too
void RestoreQuads(PPCContext& ctx, uint8_t*) {
    if (!uint8_t(ctx.r[29])) return;
    ShiftCorners(uint32_t(ctx.r[6]));
    for (uint32_t n = 0; n < (uint8_t(ctx.r[22]) ? 2u : 1u); n++) {
        ShiftCoords(uint32_t(ctx.r[6]), n, uint32_t(ctx.r[1]) + kEditorDescs[n]);
    }
}
