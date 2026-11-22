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
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "common/fps_counter.h"
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
// UI widgets: +4 name crc, +8 the element
constexpr uint32_t kWidgetName = 0x04;
constexpr uint32_t kWidgetElement = 0x08;
constexpr int kMenuWidth = 640;  // every menu layout
constexpr size_t kMaxPlaced = 4096;

std::atomic<uint32_t> g_renderer{0};
std::atomic<int> g_wide{-1};  // the mode the device started in, -1 before

thread_local float g_anchor = 0.5f;  // share of the spare width left of what is drawn
thread_local bool g_text = false;    // a glyph is drawing
thread_local bool g_paint = false;   // the red paint is drawing
thread_local bool g_black = false;   // the black paint is drawing
thread_local bool g_held = false;    // a menu part above has its place

// a kStretchWidgets part drawing: page x of its centre, -1 when none
struct Stretch {
    float centre = -1.0f;
    float place = 0.0f;  // back buffer x the narrowed layout gives the centre
};
thread_local Stretch g_stretch;

// a widget's quads in the layout's virtual x, gathered while it draws, and its glyphs alone
struct Span {
    uint32_t widget = 0;
    float left = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    float text_left = std::numeric_limits<float>::max();
    float text_right = std::numeric_limits<float>::lowest();
};

// a widget as drawn last frame: its span and the anchor its quads used
struct Placed {
    Span span;
    float anchor = 0.5f;
    float frozen = -1.0f;  // a kFrozenWidgets hold, kept from its first
};

thread_local std::vector<Span> g_open;  // the widgets drawing now, outermost first
std::mutex g_placed_lock;
std::unordered_map<uint32_t, Placed> g_placed;

// HUD widgets that sit against a screen side, by crc32 of their name: ai_status-base, -Valerie, -Doyle, -Kade
constexpr uint32_t kLeftWidgets[] = {0x17C753E5u, 0xA4F0EADFu, 0x4DA638F6u, 0xCF7E37C7u};
// ai_ammo-count1 and the unnamed count beside it, ai_objective-item, ai_live_event
constexpr uint32_t kRightWidgets[] = {0x2909DFF7u, 0xB0008E4Du, 0xDC33DFAAu, 0x0A5255EEu};
// the menus' red paint image, RedBar.xbt
constexpr uint32_t kRedPaintWidget = 0xA4E1836Eu;
// the press start screen's black paint, RedBar.xbt turned and drawn black, unnamed
constexpr uint32_t kBlackPaintWidgets[] = {0x8EFD67A5u, 0xBCE7140Du};
// menu parts that stay about the middle: the help box (A_Help, A_InfoText), the controller map's label groups and its
// page list, the objective screen's header and list (ai_main, l_objectives)
constexpr uint32_t kStayWidgets[] = {0x46740CFEu, 0xAD70BF0Fu, 0x5DDFF3C6u, 0xC3577984u, 0x221F5C34u,
                                     0xBB798CDFu, 0x32F1CF1Eu};
// parts whose text changes as they are used keep the hold they first took, else typing or the turning letter ring
// moves them: the virtual keyboard's A_VirtualKeyboard, A_Cursor, A_Cursor1 and L_Text
constexpr uint32_t kFrozenWidgets[] = {0x32971D5Cu, 0x26994A5Fu, 0x782E3A98u, 0xC9E6E44Bu};
// drawn for the console's 16:9 stretch at every aspect: the profile's ai_merc
constexpr uint32_t kStretchWidgets[] = {0x6BF79324u};
constexpr float kConsoleStretch = 4.0f / 3.0f;  // a 4:3 page across 16:9
// font: +0x54 glyphs, 0x18 each with u, v at +8, +0xC (whole part of u the texture page); +0x58 char map, a u16
// glyph index table per high byte
constexpr uint32_t kFontGlyphs = 0x54;
constexpr uint32_t kFontCharMap = 0x58;
constexpr uint32_t kGlyphSize = 0x18;
constexpr uint32_t kGlyphU = 0x08;
constexpr uint32_t kGlyphV = 0x0C;
constexpr int kPadGlyphs = 31;  // UIGlyph_table

// the drawing font's pad button glyphs by the texture corner its quads carry, u bits high, v bits low
thread_local uint64_t g_pad_icons[kPadGlyphs];
thread_local int g_pad_icon_count = 0;

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

template <size_t N>
bool In(uint32_t name, const uint32_t (&list)[N]) {
    return std::find(std::begin(list), std::end(list), name) != std::end(list);
}

int64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

}  // namespace

namespace fcip {

namespace {

std::atomic<uint64_t> g_view{0};  // x, y, width and height, 16 bits each

Box ViewBox() {
    uint64_t v = g_view;
    return {float(v & 0xFFFF), float(v >> 16 & 0xFFFF), float(v >> 32 & 0xFFFF), float(v >> 48)};
}

}  // namespace

void SetViewBox(const Box& view) {
    auto part = [](float f) { return uint64_t(std::clamp(f, 0.0f, 65535.0f)); };
    g_view = part(view.x) | part(view.y) << 16 | part(view.w) << 32 | part(view.h) << 48;
}

Box FrameBox(float w, float h) {
    Box area = ViewBox();
    if (area.w <= 0.0f || area.h <= 0.0f) area = {0.0f, 0.0f, w, h};
    // a view set for a larger window waits a frame for the new layout
    area.w = std::min(area.w, w - area.x);
    area.h = std::min(area.h, h - area.y);
    float aspect = DisplayAspect();
    float fw = std::max(std::min(area.w, area.h * aspect), 0.0f);
    float fh = fw / aspect;
    return {area.x + (area.w - fw) * 0.5f, area.y + (area.h - fh) * 0.5f, fw, fh};
}

// Output() may read the registry, so it is cached per setting and refreshed each second
float DisplayAspect() {
    Box view = ViewBox();
    if (view.w > 0.0f && view.h > 0.0f) return std::max(view.w / view.h, 4.0f / 3.0f);
    static std::atomic<float> aspect{16.0f / 9.0f};
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<int64_t> due{0};
    const app::Settings& s = app::Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    int64_t now = NowMs();
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

float WidgetAnchor(uint32_t widget) {
    std::lock_guard lock(g_placed_lock);
    auto it = g_placed.find(widget);
    return it == g_placed.end() ? 0.5f : it->second.anchor;
}

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
PPC_EXTERN_FUNC(GUEST_ORIG(Font_Draw));
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

static bool MenuLayout() {
    uint32_t ui = R32(SYM_UISystem_pointer);
    return ui && int16_t(R16(ui + kUiVirtualW)) == kMenuWidth;
}

// held at its nearer page side, or its middle when its gaps to the sides are within an eighth of a page
static float HeldAnchor(float left, float right) {
    float page = float(kMenuWidth);
    float gap_left = left;
    float gap_right = page - right;
    if (std::fabs(gap_left - gap_right) < page / 8.0f) return (left + right) * 0.5f / page;
    return (gap_left < gap_right ? left : right) / page;
}

// A menu control (button, slider, list), or a part whose text spans under 60% of the page, keeps the place the
// stretched console layout gave it, at its own shape. A part reaching a page side is held by its text, so the frame
// around it does not move it. Its span is last frame's, as a part's size is known only once it has drawn.
static void HoldInPlace(uint32_t widget) {
    if (!MenuLayout() || fcip::Scale2D() >= 1.0f) return;
    Span span;
    bool freeze = In(R32(widget + kWidgetName), kFrozenWidgets);
    {
        std::lock_guard lock(g_placed_lock);
        auto it = g_placed.find(widget);
        if (it == g_placed.end()) return;
        span = it->second.span;
        if (freeze && it->second.frozen >= 0.0f) {
            g_anchor = it->second.frozen;
            g_held = true;
            return;
        }
    }
    uint32_t element = R32(widget + kWidgetElement);
    bool control = R32(widget) == SYM_UIWidgetFocus_vtable || (element && R32(element) == SYM_UIList_vtable);
    bool text = span.text_right > span.text_left;
    bool part = text && span.text_right - span.text_left < kMenuWidth * 0.6f;
    bool frame = span.left <= 0.5f || span.right >= kMenuWidth - 0.5f;
    if (span.right <= span.left || (!control && !part) || (frame && !part)) return;
    if (frame) {
        g_anchor = HeldAnchor(span.text_left, span.text_right);
    } else {
        g_anchor = HeldAnchor(span.left, span.right);
    }
    g_held = true;
    if (!freeze) return;
    std::lock_guard lock(g_placed_lock);
    g_placed[widget].frozen = g_anchor;
}

static bool LastSpan(uint32_t widget, Span& span) {
    std::lock_guard lock(g_placed_lock);
    auto it = g_placed.find(widget);
    if (it == g_placed.end()) return false;
    span = it->second.span;
    return true;
}

// a stretched part keeps the console's shape about its centre, at the place the narrowed layout gives it
static void UpdateStretch(uint32_t widget, uint32_t name) {
    Span span;
    if (g_stretch.centre >= 0.0f || !In(name, kStretchWidgets) || !MenuLayout()) return;
    if (!LastSpan(widget, span) || span.right <= span.left) return;
    float width = float(R32(SYM_DisplayBlock + kBlockWidth));
    float k = fcip::Scale2D();
    g_stretch.centre = (span.left + span.right) * 0.5f;
    g_stretch.place = g_stretch.centre * width / kMenuWidth * k + g_anchor * (1.0f - k) * width;
}

static void Remember(const Span& span, float anchor) {
    std::lock_guard lock(g_placed_lock);
    if (g_placed.size() >= kMaxPlaced) g_placed.clear();
    Placed& placed = g_placed[span.widget];
    placed.span = span;
    placed.anchor = anchor;
}

// r3 the widget; children draw inside, so a side or a place holds for the whole part
PPC_FUNC(GUEST(UIWidget_Draw)) {
    uint32_t widget = uint32_t(ctx.r[3]);
    uint32_t name = R32(widget + kWidgetName);
    float outer = g_anchor;
    bool outer_held = g_held;
    Stretch outer_stretch = g_stretch;
    if (In(name, kLeftWidgets)) g_anchor = 0.0f;
    if (In(name, kRightWidgets)) g_anchor = 1.0f;
    if (In(name, kLeftWidgets) || In(name, kRightWidgets) || In(name, kStayWidgets)) g_held = true;
    if (!g_held) HoldInPlace(widget);
    UpdateStretch(widget, name);
    g_paint = name == kRedPaintWidget;
    g_black = In(name, kBlackPaintWidgets);
    g_open.push_back({widget});
    GUEST_ORIG(UIWidget_Draw)(ctx, base);
    Remember(g_open.back(), g_anchor);
    g_open.pop_back();
    g_paint = false;
    g_black = false;
    g_held = outer_held;
    g_anchor = outer;
    g_stretch = outer_stretch;
}

// as Font_Draw works out a glyph's corner: u and v less the page
static void NotePadIcons(uint32_t font) {
    g_pad_icon_count = 0;
    uint32_t glyphs = R32(font + kFontGlyphs);
    for (int i = 0; glyphs && i < kPadGlyphs; i++) {
        uint32_t code = R16(SYM_UIGlyph_table + uint32_t(i) * 2);
        uint32_t map = R32(font + kFontCharMap + (code >> 8) * 4);
        if (!map) continue;
        uint32_t glyph = glyphs + R16(map + (code & 0xFF) * 2) * kGlyphSize;
        float u = Float(R32(glyph + kGlyphU));
        float v = Float(R32(glyph + kGlyphV));
        float page = float(int(u));
        g_pad_icons[g_pad_icon_count++] = uint64_t(Bits(u - page)) << 32 | Bits(v - page);
    }
}

// r8 points at the quad's first texture corner
static bool IsPadIcon(uint32_t corner) {
    uint64_t uv = uint64_t(R32(corner)) << 32 | R32(corner + 4);
    return std::find(g_pad_icons, g_pad_icons + g_pad_icon_count, uv) != g_pad_icons + g_pad_icon_count;
}

// r3 the font
PPC_FUNC(GUEST(Font_Draw)) {
    fcip::NoteFontDraw(ctx);
    NotePadIcons(uint32_t(ctx.r[3]));
    g_text = true;
    GUEST_ORIG(Font_Draw)(ctx, base);
    g_text = false;
}

// a corner's x scaled by k with pos the share of the spare width before it; when narrowed, a corner at or past a page
// edge goes with that edge, so the paint, panels and lines stretch out while text and logos keep their shape
static float Fit(float x, float k, float pos, float width) {
    bool edge = !g_text && !g_paint && k < 1.0f && (x <= 1.5f || x >= width - 2.5f);
    if (edge) return x;
    return (x + 0.5f) * k + pos * (1.0f - k) * width - 0.5f;
}

// x at the console's stretch about the stretched part's centre
static float Stretched(float x, float k, float width) {
    float centre = g_stretch.centre * width / kMenuWidth;
    return g_stretch.place + (x + 0.5f - centre) * k * kConsoleStretch - 0.5f;
}

// the red paint keeps its shape at the share of the spare width it had, so a swipe moves smoothly and paint running
// off a side stays against it
static float PaintSide(const uint32_t (&x)[4], float width) {
    float lo = Float(x[0]);
    float hi = lo;
    for (uint32_t v : x) {
        lo = std::min(lo, Float(v));
        hi = std::max(hi, Float(v));
    }
    float spare = width - (hi - lo);
    if (spare < 1.0f) return 0.5f;
    return std::clamp((lo + 0.5f) / spare, 0.0f, 1.0f);
}

// the quad's x, as the game placed it, into the spans of the widgets drawing it
static void Gather(const uint32_t (&x)[4], float width) {
    float to_page = float(kMenuWidth) / width;
    for (uint32_t v : x) {
        float page_x = (Float(v) + 0.5f) * to_page;
        for (Span& span : g_open) {
            span.left = std::min(span.left, page_x);
            span.right = std::max(span.right, page_x);
            if (!g_text) continue;
            span.text_left = std::min(span.text_left, page_x);
            span.text_right = std::max(span.text_right, page_x);
        }
    }
}

// every image, rect and glyph of the UI: r4 to r7 point at the corners, {x, y} in back buffer pixels; x is scaled
// about the anchor
PPC_FUNC(GUEST(UI_DrawQuad)) {
    float scale = fcip::Scale2D();
    // 854x480 is 16:9 to within a pixel; menus still stretch their pad buttons and kStretchWidgets
    if (std::fabs(scale - 1.0f) < 0.002f && !MenuLayout()) {
        GUEST_ORIG(UI_DrawQuad)(ctx, base);
        return;
    }
    float width = float(R32(SYM_DisplayBlock + kBlockWidth));
    uint32_t corner[4] = {uint32_t(ctx.r[4]), uint32_t(ctx.r[5]), uint32_t(ctx.r[6]), uint32_t(ctx.r[7])};
    uint32_t saved[4];
    for (int i = 0; i < 4; i++) saved[i] = R32(corner[i]);
    Gather(saved, width);
    // turned across a page side, it stretches as on the console so both its ends stay past the screen edges
    if (g_black) {
        GUEST_ORIG(UI_DrawQuad)(ctx, base);
        return;
    }
    float anchor = g_paint ? PaintSide(saved, width) : g_anchor;
    float fitted[4];
    for (int i = 0; i < 4; i++) {
        float x = Float(saved[i]);
        fitted[i] = g_stretch.centre >= 0.0f ? Stretched(x, scale, width) : Fit(x, scale, anchor, width);
    }
    // the menu font's pad buttons take the console's 16:9 stretch about their centre at every aspect
    if (g_text && MenuLayout() && IsPadIcon(uint32_t(ctx.r[8]))) {
        float mid = (*std::min_element(fitted, fitted + 4) + *std::max_element(fitted, fitted + 4)) * 0.5f;
        for (float& x : fitted) x = mid + (x - mid) * kConsoleStretch;
    }
    for (int i = 0; i < 4; i++) W32(corner[i], Bits(fitted[i]));
    GUEST_ORIG(UI_DrawQuad)(ctx, base);
    for (int i = 0; i < 4; i++) W32(corner[i], saved[i]);
}
