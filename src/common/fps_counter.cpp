// The fps counter, drawn by the game in its own menu font at the top left, into the overlay batch the renderer
// flushes last, and the loading screen's shader archive progress, opposite its label in the label's own style
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/fps_counter.h"

#include "common/aspect.h"
#include "common/settings.h"
#include "d3d/fsr.h"
#include "d3d/shader_archive.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr const char* kFontName = "Geometr706 BdCn BT Bold 20";
constexpr uint32_t kBatch = 0x91F0;  // the renderer's batch, and its overlay batch
constexpr uint32_t kOverlayBatch = 0x91EC;
constexpr uint32_t kPassWidth = 0x1C;  // a pass the size of the back buffer counts as a frame
constexpr uint32_t kPassHeight = 0x20;
constexpr uint32_t kEngineWidth = 0x13FC;
constexpr uint32_t kEngineHeight = 0x1400;
constexpr uint32_t kFontSize = 0x49;     // u8
// glyph records of 0x18 bytes, advance u8 at +6; per 256 characters a u16 table of record numbers
constexpr uint32_t kFontGlyphs = 0x54;
constexpr uint32_t kFontPages = 0x58;
constexpr uint32_t kGlyphBytes = 0x18;
constexpr uint32_t kGlyphAdvance = 6;
constexpr uint32_t kUiVirtualW = 0x58;  // s16
constexpr uint32_t kUiVirtualH = 0x5A;  // s16
constexpr uint32_t kUnits = 32;
constexpr float kScale = 0.5f;
constexpr uint32_t kStyleBytes = 16;
constexpr uint32_t kMaxLabelUnits = 64;

uint32_t g_font = 0;
uint32_t g_block = 0;  // the style, then the text
thread_local bool g_loading = false;

// the loading screen's label as the screen last drew it, one word in every language; its tip is a sentence
struct Label {
    uint32_t font = 0;
    uint8_t style[kStyleBytes] = {};
    uint32_t y = 0;
    uint32_t arg = 0;  // the draw's sixth argument
    uint32_t shadow = 0;
    double f1 = 0;
};
Label g_label;

// the text into our block after the style, its unit count back
uint32_t PutText(const char* text) {
    uint32_t str = g_block + kStyleBytes;
    uint32_t n = 0;
    for (; text[n] && n + 1 < kUnits; n++) W16(str + n * 2, uint8_t(text[n]));
    W16(str + n * 2, 0);
    return n;
}

// style {f32 scale, u32 colour with alpha in the top byte, f32 spacing, u8 flags}
void Draw(PPCContext& ctx, const char* text, int x, int y, uint32_t colour, bool shadow) {
    uint32_t style = g_block;
    uint32_t scale;
    std::memcpy(&scale, &kScale, 4);
    W32(style, scale);
    W32(style + 4, colour);
    W32(style + 8, 0);
    W32(style + 12, 0);
    uint32_t n = PutText(text);
    ctx.f[1] = 0.0;
    CallGuest(ctx, SYM_Font_Draw, {g_font, style + kStyleBytes, uint32_t(x), uint32_t(y), style, 0, n,
                                   shadow ? 1u : 0u});
}

// as the font draw advances across the text: each glyph's advance times the scale, plus the spacing
float TextWidth(uint32_t font, const char* text, float scale, float spacing) {
    float width = 0;
    for (const char* c = text; *c; c++) {
        uint8_t unit = uint8_t(*c);
        uint32_t page = R32(font + kFontPages);
        uint32_t glyph = page ? R16(page + unit * 2u) : 0;
        width += R8(R32(font + kFontGlyphs) + glyph * kGlyphBytes + kGlyphAdvance) * scale + spacing;
    }
    return width;
}

float StyleFloat(const uint8_t* style, uint32_t at) {
    uint32_t bits = uint32_t(style[at]) << 24 | style[at + 1] << 16 | style[at + 2] << 8 | style[at + 3];
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// (font, text, x, y from the bottom, style, ?, length, shadow): a one word text the loading screen draws while it
// holds for the archive is its label
void NoteLabel(PPCContext& ctx) {
    uint32_t str = uint32_t(ctx.r[4]);
    uint32_t n = std::min(uint32_t(ctx.r[9]), kMaxLabelUnits);
    if (!g_block || str == g_block + kStyleBytes || !n) return;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t unit = R16(str + i * 2);
        if (!unit) break;
        if (unit == u' ') return;
    }
    g_label.font = uint32_t(ctx.r[3]);
    g_label.y = uint32_t(ctx.r[6]);
    g_label.arg = uint32_t(ctx.r[8]);
    g_label.shadow = uint32_t(ctx.r[10]);
    g_label.f1 = ctx.f[1];
    std::memcpy(g_label.style, HostPtr(uint32_t(ctx.r[7])), kStyleBytes);
}

double Fps() {
    static auto start = std::chrono::steady_clock::now();
    static uint32_t frames = 0;
    static double fps = 0.0;
    frames++;
    auto now = std::chrono::steady_clock::now();
    double span = std::chrono::duration<double>(now - start).count();
    if (span >= 0.5) {
        fps = frames / span;
        frames = 0;
        start = now;
    }
    return fps;
}

void DrawFps(PPCContext& ctx) {
    char text[32];
    double fps = Fps();
    if (uint32_t n = fsr::Generated()) std::snprintf(text, sizeof(text), "FPS: %.1f, FG %.1f", fps, fps * (n + 1));
    else std::snprintf(text, sizeof(text), "FPS: %.1f", fps);
    // y is up from the bottom, so the baseline sits at height minus y
    uint32_t ui = R32(SYM_UISystem_pointer);
    int size = int(R8(g_font + kFontSize) * kScale);
    int margin = size / 2;
    int x = margin;
    int y = int16_t(R16(ui + kUiVirtualH)) - margin - size;
    fcip::Anchor2D(0.0f);
    Draw(ctx, text, x + 1, y - 1, 0xFF000000u, true);
    Draw(ctx, text, x, y, 0xFFFFFFFFu, false);
    fcip::Anchor2D(0.5f);
}

// centred across the screen on the label's line, in the label's font and style
void DrawProgress(PPCContext& ctx, int percent) {
    char text[32];
    std::snprintf(text, sizeof(text), "COMPILING SHADERS: %d%%", percent);
    uint32_t ui = R32(SYM_UISystem_pointer);
    float width = TextWidth(g_label.font, text, StyleFloat(g_label.style, 0), StyleFloat(g_label.style, 8));
    int x = (int16_t(R16(ui + kUiVirtualW)) - int(width)) / 2;
    std::memcpy(HostPtr(g_block), g_label.style, kStyleBytes);
    uint32_t n = PutText(text);
    ctx.f[1] = g_label.f1;
    CallGuest(ctx, SYM_Font_Draw, {g_label.font, g_block + kStyleBytes, uint32_t(x), g_label.y, g_block,
                                   g_label.arg, n, g_label.shadow});
}

}  // namespace

void fcip::NoteFontDraw(PPCContext& ctx) {
    if (d3d::ShaderArchivePercent() >= 0) NoteLabel(ctx);
}

PPC_EXTERN_FUNC(GUEST_ORIG(Font_Load));
PPC_EXTERN_FUNC(GUEST_ORIG(Overlay_Flush));

// our own copy of the menu font the first time a scene loads it, so no scene unload frees it
PPC_FUNC(GUEST(Font_Load)) {
    uint32_t dir = uint32_t(ctx.r[4]);
    uint32_t name = uint32_t(ctx.r[5]);
    GUEST_ORIG(Font_Load)(ctx, base);
    if (g_loading || g_font || uint32_t(ctx.r[3]) != 1) return;
    if (std::strcmp(static_cast<const char*>(HostPtr(name)), kFontName)) return;
    g_block = g_block ? g_block : mem::AllocSystem(16 + kUnits * 2);
    if (!g_block) return;
    uint32_t font = uint32_t(CallGuest(ctx, SYM_Font_New, {0, 0}));
    if (!font) return;
    g_loading = true;
    bool loaded = CallGuest(ctx, SYM_Font_Load, {font, dir, name, 0}) == 1;
    g_loading = false;
    if (loaded) g_font = font;
}

// r3 the renderer, r4 the pass
PPC_FUNC(GUEST(Overlay_Flush)) {
    uint32_t renderer = uint32_t(ctx.r[3]);
    uint32_t pass = uint32_t(ctx.r[4]);
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    bool frame = g_font && fcip::Get().fps_counter && engine && pass &&
                 R32(pass + kPassWidth) == R32(engine + kEngineWidth) &&
                 R32(pass + kPassHeight) == R32(engine + kEngineHeight);
    int percent = d3d::ShaderArchivePercent();
    bool compiling = percent >= 0 && g_block && g_label.font;
    if (!frame && !compiling) {
        GUEST_ORIG(Overlay_Flush)(ctx, base);
        return;
    }
    uint32_t batch = R32(renderer + kBatch);
    W32(renderer + kBatch, R32(renderer + kOverlayBatch));
    if (frame) DrawFps(ctx);
    if (compiling) DrawProgress(ctx, percent);
    W32(renderer + kBatch, batch);
    GUEST_ORIG(Overlay_Flush)(ctx, base);
}
