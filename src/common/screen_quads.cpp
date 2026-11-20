// The game's screen quads put their edges on pixel centres: the post effects' copies (default.xex 8237BB08, ige.xex
// 82481E28) and blur (8237B268, 824815E8) map pixel -0.5 to the screen's edge, the full screen quad (823AE398,
// 82372C08) that restores EDRAM starts at the viewport's corners, and 2D images (82363548, 8235E148) such as
// the level fades start at pixel 0. Upscaled, the half pixel shift then leaves the top row and left column of host
// pixels out. Each quad moves half a pixel up and left, a 2D image only its corners on the screen's edges, texture
// coordinates with them, so it covers every host pixel and samples the same texels.
#include <bit>
#include <initializer_list>

#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kVertices = 4;
// the display block: the full screen quad's viewport
constexpr uint32_t kBlockViewWidth = 0x198;
constexpr uint32_t kBlockViewHeight = 0x19C;
// vertex layouts: x, y first, then texture coordinate pairs
constexpr uint32_t kPostStride = 0x1C;
constexpr uint32_t kPostCoords = 0xC;
constexpr uint32_t kPostCoords2 = 0x14;
constexpr uint32_t kBlurStride = 0x14;
constexpr uint32_t kBlurCoords = 0xC;
constexpr uint32_t kFullStride = 0x18;  // x, y in clip space
constexpr uint32_t kFullCoords = 0x8;
constexpr uint32_t kFullCoords2 = 0x10;
constexpr uint32_t kImageVertices = 0x70;  // on the 2D image's stack
constexpr uint32_t kImageStride = 0x20;
constexpr uint32_t kImageCoords = 0xC;

float ReadFloat(uint32_t at) { return std::bit_cast<float>(R32(at)); }
void AddFloat(uint32_t at, float add) { W32(at, std::bit_cast<uint32_t>(ReadFloat(at) + add)); }

// moves an axis aligned quad by dx, dy, each coordinate pair at coords along its own gradient; a turned quad stays
void ShiftQuad(uint32_t vertices, uint32_t stride, float dx, float dy, std::initializer_list<uint32_t> coords) {
    float x0 = ReadFloat(vertices);
    float y0 = ReadFloat(vertices + 4);
    uint32_t row = 0;
    uint32_t column = 0;
    for (uint32_t i = 1; i < kVertices; i++) {
        float x = ReadFloat(vertices + i * stride);
        float y = ReadFloat(vertices + i * stride + 4);
        if (y == y0 && x != x0) row = vertices + i * stride;
        if (x == x0 && y != y0) column = vertices + i * stride;
    }
    if (!row || !column) return;
    for (uint32_t c : coords) {
        float du = (ReadFloat(row + c) - ReadFloat(vertices + c)) / (ReadFloat(row) - x0);
        float dv = (ReadFloat(column + c + 4) - ReadFloat(vertices + c + 4)) / (ReadFloat(column + 4) - y0);
        for (uint32_t i = 0; i < kVertices; i++) {
            AddFloat(vertices + i * stride + c, du * dx);
            AddFloat(vertices + i * stride + c + 4, dv * dy);
        }
    }
    for (uint32_t i = 0; i < kVertices; i++) {
        AddFloat(vertices + i * stride, dx);
        AddFloat(vertices + i * stride + 4, dy);
    }
}

// corners on the screen's left or top edge half a pixel out, each coordinate extrapolated from a corner on its line
void ExtendToEdges(uint32_t vertices, uint32_t stride, uint32_t coords) {
    float x[kVertices];
    float y[kVertices];
    float u[kVertices];
    float v[kVertices];
    for (uint32_t i = 0; i < kVertices; i++) {
        uint32_t at = vertices + i * stride;
        x[i] = ReadFloat(at);
        y[i] = ReadFloat(at + 4);
        u[i] = ReadFloat(at + coords);
        v[i] = ReadFloat(at + coords + 4);
    }
    for (uint32_t i = 0; i < kVertices; i++) {
        uint32_t at = vertices + i * stride;
        for (uint32_t j = 0; j < kVertices; j++) {
            if (x[i] == 0 && y[j] == y[i] && x[j] != 0) {
                AddFloat(at, -0.5f);
                AddFloat(at + coords, -0.5f * (u[j] - u[i]) / x[j]);
            }
            if (y[i] == 0 && x[j] == x[i] && y[j] != 0) {
                AddFloat(at + 4, -0.5f);
                AddFloat(at + coords + 4, -0.5f * (v[j] - v[i]) / y[j]);
            }
        }
    }
}

void ShiftPost(uint32_t vertices) { ShiftQuad(vertices, kPostStride, -0.5f, -0.5f, {kPostCoords, kPostCoords2}); }

// the blur draws one vertex array every pass: moved for its draw, then put back
thread_local uint32_t t_blur[kVertices * kBlurStride / 4];
thread_local uint32_t t_blur_at = 0;

void ShiftBlur(uint32_t vertices) {
    for (uint32_t i = 0; i < kVertices * kBlurStride / 4; i++) t_blur[i] = R32(vertices + i * 4);
    t_blur_at = vertices;
    ShiftQuad(vertices, kBlurStride, -0.5f, -0.5f, {kBlurCoords});
}

// clip space y points up
void ShiftFull(uint32_t vertices, std::initializer_list<uint32_t> coords) {
    float w = float(R32(SYM_DisplayBlock + kBlockViewWidth));
    float h = float(R32(SYM_DisplayBlock + kBlockViewHeight));
    if (w > 0 && h > 0) ShiftQuad(vertices, kFullStride, -1 / w, 1 / h, coords);
}

}  // namespace

// before each draw, the quad's vertices in r6 (default.xex) or r5 (ige.xex)
void PostQuad(PPCContext& ctx, uint8_t*) { ShiftPost(uint32_t(ctx.r[6])); }
void EditorPostQuad(PPCContext& ctx, uint8_t*) { ShiftPost(uint32_t(ctx.r[5])); }
void BlurQuad(PPCContext& ctx, uint8_t*) { ShiftBlur(uint32_t(ctx.r[6])); }
void EditorBlurQuad(PPCContext& ctx, uint8_t*) { ShiftBlur(uint32_t(ctx.r[5])); }

// a 2D image's vertices built, r1 its stack
void ImageQuad(PPCContext& ctx, uint8_t*) {
    ExtendToEdges(uint32_t(ctx.r[1]) + kImageVertices, kImageStride, kImageCoords);
}

void BlurQuadDone(PPCContext&, uint8_t*) {
    if (!t_blur_at) return;
    for (uint32_t i = 0; i < kVertices * kBlurStride / 4; i++) W32(t_blur_at + i * 4, t_blur[i]);
    t_blur_at = 0;
}

// r23 set when it copies a texture
void RestoreQuad(PPCContext& ctx, uint8_t*) {
    if (uint8_t(ctx.r[23])) ShiftFull(uint32_t(ctx.r[6]), {kFullCoords});
    else ShiftFull(uint32_t(ctx.r[6]), {});
}

// r29 set when it copies a texture, r22 when a second one too
void RestoreQuads(PPCContext& ctx, uint8_t*) {
    uint32_t vertices = uint32_t(ctx.r[6]);
    if (!uint8_t(ctx.r[29])) ShiftFull(vertices, {});
    else if (!uint8_t(ctx.r[22])) ShiftFull(vertices, {kFullCoords});
    else ShiftFull(vertices, {kFullCoords, kFullCoords2});
}
