// The frame takes the output's shape. The game's own mode, 16:9 or 4:3, only picks its 2D set. Each layout keeps the
// shape it was made for: narrower by as much as the frame is wider than it, wider by as much as it is narrower, about
// the side it sits against, so the HUD keeps its offsets from the screen edges.
#include "common/aspect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

#include "app/app.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// display block: +1 widescreen byte, +4/+8 back buffer size, +0xC display aspect, +0x44 present flags (bit 0 in 4:3)
constexpr uint32_t kBlockWide = 0x01;
constexpr uint32_t kBlockWidth = 0x04;
constexpr uint32_t kBlockHeight = 0x08;
constexpr uint32_t kBlockAspect = 0x0C;
constexpr uint32_t kBlockFlags = 0x44;
// the renderer copies the block at init: +0x35C mode (2 wide), +0x360 aspect, +0x364 pixel aspect
constexpr uint32_t kRendererAspect = 0x360;
constexpr uint32_t kRendererPixelAspect = 0x364;
// UI system: the virtual size of the last loaded layout, which its draws are scaled from to the back buffer
constexpr uint32_t kUiVirtualW = 0x58;  // s16
constexpr uint32_t kUiVirtualH = 0x5A;

std::atomic<uint32_t> g_renderer{0};
std::atomic<int> g_wide{-1};  // the mode the device started in, -1 before

thread_local float g_anchor = 0.5f;  // share of the spare width left of what is drawn
thread_local bool g_rect = false;    // a rect element is drawing

// HUD widgets that sit against a screen side, by crc32 of their name: ai_status-base, -Valerie, -Doyle, -Kade
constexpr uint32_t kLeftWidgets[] = {0x17C753E5u, 0xA4F0EADFu, 0x4DA638F6u, 0xCF7E37C7u};
// ai_ammo-count1 and the unnamed count beside it, ai_objective-item, ai_live_event
constexpr uint32_t kRightWidgets[] = {0x2909DFF7u, 0xB0008E4Du, 0xDC33DFAAu, 0x0A5255EEu};

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

float Float(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// The mode picks which asset packages load, so it only changes at device init: live, a 4:3 mode over the loaded wide
// packages finds no CryLoadingMaster and crashes.
void WriteMode(float aspect) {
    uint32_t block = SYM_DisplayBlock;
    W32(block + kBlockAspect, Bits(aspect));
    bool wide = fcip::DisplayWide();
    W8(block + kBlockWide, wide ? 1 : 0);
    uint32_t flags = R32(block + kBlockFlags);
    W32(block + kBlockFlags, wide ? flags & ~1u : flags | 1u);
}

void WriteRenderer(float aspect, uint32_t renderer) {
    uint32_t block = SYM_DisplayBlock;
    float pixel = aspect / float(double(R32(block + kBlockWidth)) / double(R32(block + kBlockHeight)));
    W32(block + kBlockAspect, Bits(aspect));
    W32(renderer + kRendererAspect, Bits(aspect));
    W32(renderer + kRendererPixelAspect, Bits(pixel));
}

bool In(uint32_t name, const uint32_t (&list)[4]) { return std::find(std::begin(list), std::end(list), name) != std::end(list); }

}  // namespace

namespace fcip {

// Output() may read the registry, so it is cached per setting and refreshed each second
float DisplayAspect() {
    static std::atomic<float> aspect{16.0f / 9.0f};
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<int64_t> due{0};
    const app::Settings& s = app::Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
    if (key.exchange(k) != k || now >= due) {
        app::Mode m = app::Output();
        aspect = m.height ? std::max(float(m.width) / float(m.height), 4.0f / 3.0f) : 4.0f / 3.0f;
        due = now + 1000;
    }
    return aspect;
}

// as the console reports the TV
bool DisplayWide() {
    int wide = g_wide;
    if (wide >= 0) return wide != 0;
    return app::Widescreen();
}

// menus are 640x480 in both modes, the 16:9 HUD and loading screens 854x480
static float LayoutAspect() {
    uint32_t ui = R32(SYM_UISystem_pointer);
    int w = ui ? int16_t(R16(ui + kUiVirtualW)) : 0;
    int h = ui ? int16_t(R16(ui + kUiVirtualH)) : 0;
    if (w > 0 && h > 0) return float(w) / float(h);
    return DisplayWide() ? 16.0f / 9.0f : 4.0f / 3.0f;
}

float Scale2D() { return LayoutAspect() / DisplayAspect(); }

void Anchor2D(float side) { g_anchor = side; }

void AspectTick() {
    static std::atomic<float> applied{0.0f};
    uint32_t renderer = g_renderer;
    float aspect = DisplayAspect();
    if (renderer && applied.exchange(aspect) != aspect) WriteRenderer(aspect, renderer);
}

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(Device_Init));
PPC_EXTERN_FUNC(GUEST_ORIG(Renderer_Init));
PPC_EXTERN_FUNC(GUEST_ORIG(Bink_DrawFrame));
PPC_EXTERN_FUNC(GUEST_ORIG(UIWidget_Draw));
PPC_EXTERN_FUNC(GUEST_ORIG(UIRect_Draw));
PPC_EXTERN_FUNC(GUEST_ORIG(UI_DrawQuad));

PPC_FUNC(GUEST(Device_Init)) {
    GUEST_ORIG(Device_Init)(ctx, base);
    if (g_wide < 0) g_wide = fcip::DisplayWide();
    WriteMode(fcip::DisplayAspect());
}

PPC_FUNC(GUEST(Renderer_Init)) {
    uint32_t renderer = uint32_t(ctx.r[3]);
    GUEST_ORIG(Renderer_Init)(ctx, base);
    g_renderer = renderer;
}

// movie frame draw (f1 x offset, f2 y offset, f3 x scale, f4 y scale): the player fills the width and takes the height
// from the display aspect, which runs past the frame above 16:9, so it is fitted to the height instead
PPC_FUNC(GUEST(Bink_DrawFrame)) {
    if (ctx.f[2] < 0.0) {
        double w = R32(SYM_DisplayBlock + kBlockWidth);
        double h = R32(SYM_DisplayBlock + kBlockHeight);
        double fit = h / (h - 2.0 * ctx.f[2]);
        ctx.f[1] = ctx.f[1] * fit + w * (1.0 - fit) * 0.5;
        ctx.f[2] = 0.0;
        ctx.f[3] *= fit;
        ctx.f[4] *= fit;
    }
    GUEST_ORIG(Bink_DrawFrame)(ctx, base);
}

// r3 the widget, its name's crc at +4; children draw inside, so a side holds for the whole part
PPC_FUNC(GUEST(UIWidget_Draw)) {
    uint32_t name = R32(uint32_t(ctx.r[3]) + 4);
    float outer = g_anchor;
    if (In(name, kLeftWidgets)) g_anchor = 0.0f;
    if (In(name, kRightWidgets)) g_anchor = 1.0f;
    GUEST_ORIG(UIWidget_Draw)(ctx, base);
    g_anchor = outer;
}

PPC_FUNC(GUEST(UIRect_Draw)) {
    g_rect = true;
    GUEST_ORIG(UIRect_Draw)(ctx, base);
    g_rect = false;
}

// a corner's x scaled by k with pos the share of the spare width before it; when narrowed, a rect's corner on a page
// edge stays there, so fills still reach the screen edges
static float Fit(float x, float k, float pos, float width) {
    if (g_rect && k < 1.0f && (x <= 1.5f || x >= width - 2.5f)) return x;
    return (x + 0.5f) * k + pos * (1.0f - k) * width - 0.5f;
}

// every image, rect and glyph of the UI: r4 to r7 point at the corners, {x, y} in back buffer pixels; x is scaled
// about the anchor
PPC_FUNC(GUEST(UI_DrawQuad)) {
    float scale = fcip::Scale2D();
    // 854x480 is 16:9 to within a pixel
    if (std::fabs(scale - 1.0f) < 0.002f) {
        GUEST_ORIG(UI_DrawQuad)(ctx, base);
        return;
    }
    float width = float(R32(SYM_DisplayBlock + kBlockWidth));
    uint32_t corner[4] = {uint32_t(ctx.r[4]), uint32_t(ctx.r[5]), uint32_t(ctx.r[6]), uint32_t(ctx.r[7])};
    uint32_t saved[4];
    for (int i = 0; i < 4; i++) saved[i] = R32(corner[i]);
    for (int i = 0; i < 4; i++) W32(corner[i], Bits(Fit(Float(saved[i]), scale, g_anchor, width)));
    GUEST_ORIG(UI_DrawQuad)(ctx, base);
    for (int i = 0; i < 4; i++) W32(corner[i], saved[i]);
}
