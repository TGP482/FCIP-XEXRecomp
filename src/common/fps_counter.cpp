// The fps counter, drawn by the game in its own menu font at the top left, into the overlay batch the renderer
// flushes last
#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/aspect.h"
#include "common/settings.h"
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
constexpr uint32_t kUiVirtualH = 0x5A;  // s16
constexpr uint32_t kUnits = 32;
constexpr float kScale = 0.5f;

uint32_t g_font = 0;
uint32_t g_block = 0;  // the style, then the text
thread_local bool g_loading = false;

// style {f32 scale, u32 colour with alpha in the top byte, f32 spacing, u8 flags}
void Draw(PPCContext& ctx, const char* text, int x, int y, uint32_t colour, bool shadow) {
    uint32_t style = g_block;
    uint32_t str = g_block + 16;
    uint32_t scale;
    std::memcpy(&scale, &kScale, 4);
    W32(style, scale);
    W32(style + 4, colour);
    W32(style + 8, 0);
    W32(style + 12, 0);
    uint32_t n = 0;
    for (; text[n] && n + 1 < kUnits; n++) W16(str + n * 2, uint8_t(text[n]));
    W16(str + n * 2, 0);
    ctx.f[1] = 0.0;
    CallGuest(ctx, SYM_Font_Draw, {g_font, str, uint32_t(x), uint32_t(y), style, 0, n, shadow ? 1u : 0u});
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

}  // namespace

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
    if (!frame) {
        GUEST_ORIG(Overlay_Flush)(ctx, base);
        return;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "FPS: %.1f", Fps());
    // y is up from the bottom, so the baseline sits at height minus y
    uint32_t ui = R32(SYM_UISystem_pointer);
    int size = int(R8(g_font + kFontSize) * kScale);
    int margin = size / 2;
    int x = margin;
    int y = int16_t(R16(ui + kUiVirtualH)) - margin - size;
    uint32_t batch = R32(renderer + kBatch);
    W32(renderer + kBatch, R32(renderer + kOverlayBatch));
    fcip::Anchor2D(0.0f);
    Draw(ctx, text, x + 1, y - 1, 0xFF000000u, true);
    Draw(ctx, text, x, y, 0xFFFFFFFFu, false);
    fcip::Anchor2D(0.5f);
    W32(renderer + kBatch, batch);
    GUEST_ORIG(Overlay_Flush)(ctx, base);
}
