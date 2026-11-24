// default.xex: an unlocked achievement shows in our copy of the HUD's contextual help box on the right side of the
// screen, written as the game writes its own hints (821FC620 from 8214C540), one at a time while playing, held for 5
// seconds with its picture left of the title and the description below it
#include "game/achievement_toast.h"

#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "common/input.h"
#include "common/menu.h"
#include "common/widgets.h"
#include "game/achievement_icons.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xam/xam.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace fcip::ui;

// the HUD as WriteHudContextualHelp (820EC378) finds it: the game's own, else the first client's by its id
constexpr uint32_t kGameHud = 0x7Cu;
constexpr uint32_t kGameClients = 0x30u;  // client pointers, the count at +0x68
constexpr uint32_t kGameClientCount = 0x68u;
constexpr uint32_t kClientHudId = 0x38u;  // low 16 bits
constexpr uint32_t kGameHuds = 0x340u;    // its +0x08 finds a HUD by id, vtable +0x38
constexpr uint32_t kFindHud = 0x38u;
constexpr uint32_t kHudHelp = 0x821FC620u;  // (hud, const std::wstring* text, 1), writes the box at kHudHelpBox
constexpr uint32_t kHudPage = 0x50u;  // the HUD page behind one pointer, holding ai_contextual_1 (821F99D4)
constexpr uint32_t kHudSprites = 0x14u;
constexpr uint32_t kHudSelector = 0x32Cu;  // ai_selector, whose i_item1 picture the icon copies

// ai_contextual_1, whose element plays the box's area: faded in by key 4, fading out from key 6
constexpr uint32_t kHudHelpBox = 0x37Cu;
constexpr uint32_t kElementArea = 0x2Cu;  // its children at [+0x08, +0x0C), frames per key at +0x1C, u16
constexpr uint32_t kAreaFramesPerKey = 0x1Cu;
constexpr uint32_t kElementFrame = 0x64u;
constexpr uint32_t kElementPlaying = 0x68u;  // u8
constexpr uint32_t kShownKey = 4;
constexpr uint32_t kFadeKey = 6;
constexpr int kBoxWidth = 256;  // its t_text
constexpr uint32_t kUiVirtualWidth = 0x58u;  // the UI system's, s16

// cl_display_hud plays the HUD page from key 20 to hide it and from key 10 to show it (821FBD18), moving each
// child along its own keys
constexpr uint32_t kWidgetTrack = 0x0Cu;  // keys at [+0x04, +0x08): state +0x04, index in the top 12 bits of +0x0C
constexpr uint32_t kMaxKeys = 64;
constexpr int kHudShowKey = 10;
constexpr int kHudHideKey = 20;

constexpr uint32_t kCrcText = 0x81FDCEB5u;   // t_text
constexpr uint32_t kCrcItem = 0xC1745286u;   // i_item1
constexpr uint32_t kCrcDesc = 0x3DB56427u;   // t_description, our copy of t_text below it
constexpr uint32_t kCrcBox = 0xC48B0B92u;    // ai_contextual_achievement, our copy of ai_contextual_1
constexpr uint32_t kSetPicture = 0x82477458u;  // (element, child name crc, sprite)
constexpr uint32_t kSetText = 0x82477168u;     // (element, child name crc, const wchar_t*)
constexpr uint32_t kTransformRect = 0x10u;     // s16 left, right, top, bottom
constexpr uint32_t kTextColour = 0x20u;        // a text state's ARGB
constexpr uint32_t kWhite = 0x00FFFFFFu;
constexpr int kIconSize = 48;
constexpr int kIconGap = 8;
constexpr int kDescTop = 24;  // a title line and a small gap

constexpr auto kHold = 5s;
constexpr auto kGap = kHold + 1s;

// our box, made once per HUD
struct Box {
    uint32_t hud = 0;
    uint32_t widget = 0;
    uint32_t element = 0;
};

std::deque<uint16_t> g_queue;
Clock::time_point g_shown;
Box g_box;
bool g_showing = false;
bool g_game_box_visible = true;  // as the game left it before ours showed
// our widgets in the box: the icon and the description, and the box's own text they fade with
struct Extras {
    uint32_t icon = 0;
    uint32_t desc = 0;
    uint32_t text = 0;
};
Extras g_extras;

const xam::Achievement* Find(const std::vector<xam::Achievement>& list, uint16_t id) {
    for (const xam::Achievement& a : list) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

uint32_t Hud(PPCContext& ctx, uint32_t game) {
    if (uint32_t hud = kernel::R32(game + kGameHud)) return hud;
    uint32_t client = kernel::R32(game + kGameClientCount) ? kernel::R32(game + kGameClients) : 0;
    uint32_t huds = kernel::R32(kernel::R32(game + kGameHuds) + 0x08);
    if (!client || !huds) return 0;
    uint32_t id = kernel::R32(client + kClientHudId) & 0xFFFF;
    return uint32_t(kernel::CallGuest(ctx, kernel::R32(kernel::R32(huds) + kFindHud), {huds, id}));
}

void SetVisible(uint32_t widget, bool on) {
    uint8_t flags = Read8(widget + kWidgetVisible);
    Write8(widget + kWidgetVisible, uint8_t(on ? flags | 0x80u : flags & ~0x80u));
}

// every key from the HUD's show on rests where the box sits before its hide, so the toggle leaves it on screen
void KeepShown(uint32_t widget) {
    uint32_t track = Read32(widget + kWidgetTrack);
    uint32_t first = Read32(track + 0x04);
    uint32_t last = Read32(track + 0x08);
    if (!AddrOk(first) || last < first || last - first > kMaxKeys * 4) return;
    auto key = [](uint32_t it) { return int32_t(Read32(Read32(it) + 0x0C)) >> 20; };
    uint32_t shown = 0;
    for (uint32_t it = first; it < last; it += 4) {
        if (key(it) < kHudHideKey) shown = Read32(Read32(it) + 0x04);
    }
    if (!AddrOk(shown)) return;
    for (uint32_t it = first; it < last; it += 4) {
        if (key(it) >= kHudShowKey) Write32(Read32(it) + 0x04, shown);
    }
}

// a copy of the selector's picture left of t_text
uint32_t MakeIcon(PPCContext& ctx, uint32_t hud, uint32_t area, const int16_t (&rect)[4]) {
    uint32_t selector = Read32(hud + kHudSelector);
    uint32_t item = FindWidget(Read32(Read32(selector + kWidgetElement) + kElementArea), kCrcItem);
    uint32_t icon = item ? CloneChild(ctx, area, item) : 0;
    if (!icon) return 0;
    SetVisible(icon, false);
    int left = rect[0];
    SetRect(icon, {int16_t(left - kIconGap - kIconSize), int16_t(left - kIconGap), rect[2],
                   int16_t(rect[2] + kIconSize)});
    return icon;
}

// a copy of t_text a line lower, named apart so it takes its own text
uint32_t MakeDesc(PPCContext& ctx, uint32_t area, uint32_t text, const int16_t (&rect)[4]) {
    uint32_t desc = CloneChild(ctx, area, text);
    if (!desc) return 0;
    SetVisible(desc, false);
    Write32(desc + kWidgetNameCrc, kCrcDesc);
    SetRect(desc, {rect[0], rect[1], int16_t(rect[2] + kDescTop), int16_t(rect[3] + kDescTop)});
    return desc;
}

Extras MakeExtras(PPCContext& ctx, uint32_t hud, uint32_t area) {
    uint32_t text = FindWidget(area, kCrcText);
    Extras extras{FindWidget(area, kCrcItem), FindWidget(area, kCrcDesc), text};
    if (!text) return extras;
    uint32_t transform = Read32(Read32(text + kWidgetElement) + 0x04);
    int16_t rect[4];
    for (uint32_t i = 0; i < 4; i++) rect[i] = ReadS16(transform + kTransformRect + i * 2);
    if (!extras.icon) extras.icon = MakeIcon(ctx, hud, area, rect);
    if (!extras.desc) extras.desc = MakeDesc(ctx, area, text, rect);
    return extras;
}

// t_text's alpha as it fades, a frame late; the icon's own states are pictures, not text, so it can't play t_text's keys
void FadeIcon() {
    uint32_t state = g_extras.text ? Read32(Read32(g_extras.text + kWidgetElement) + 0x04) : 0;
    if (!g_extras.icon || !AddrOk(state)) return;
    SetPictureColour(g_extras.icon, (Read32(state + kTextColour) & ~kWhite) | kWhite);
}

// the game's box draws the children it shares with ours even when idle, in whatever state ours left them
void HideIdleGameBox(uint32_t hud, bool showing) {
    uint32_t game_box = Read32(hud + kHudHelpBox);
    uint32_t element = AddrOk(game_box) ? Read32(game_box + kWidgetElement) : 0;
    if (AddrOk(element)) SetVisible(game_box, showing ? Read8(element + kElementPlaying) != 0 : g_game_box_visible);
}

// held at full until kHold is up, then left to fade; a new HUD needs a new box
void UpdateBox(PPCContext& ctx, uint32_t game, Clock::time_point now) {
    if (!g_box.hud) return;
    uint32_t hud = game ? Hud(ctx, game) : 0;
    if (hud != g_box.hud) {
        g_box = {};
        g_extras = {};
        g_showing = false;
        return;
    }
    if (!g_showing) return;
    HideIdleGameBox(hud, true);
    FadeIcon();
    uint32_t per_key = kernel::R16(kernel::R32(g_box.element + kElementArea) + kAreaFramesPerKey);
    uint32_t frame = kernel::R32(g_box.element + kElementFrame);
    if (now - g_shown < kHold) {
        if (frame >= per_key * (kFadeKey - 1)) kernel::W32(g_box.element + kElementFrame, per_key * kShownKey);
        return;
    }
    if (kernel::R8(g_box.element + kElementPlaying)) return;
    SetVisible(g_extras.icon, false);
    SetVisible(g_extras.desc, false);
    HideIdleGameBox(hud, false);
    g_showing = false;
}

// a copy of ai_contextual_1 in the HUD page, moved to the right; the game keeps its own box for its hints. Both boxes
// share one area of children, whose texts each element sets apart, so the game's sets our description to nothing
bool EnsureBox(PPCContext& ctx, uint32_t hud) {
    if (g_box.hud == hud) return g_box.widget != 0;
    g_box = {hud};
    g_extras = {};
    uint32_t game_box = kernel::R32(hud + kHudHelpBox);
    uint32_t page = kernel::R32(kernel::R32(hud + kHudPage));
    uint32_t widget = game_box && page ? CloneChild(ctx, page, game_box) : 0;
    uint32_t element = widget ? kernel::R32(widget + kWidgetElement) : 0;
    uint32_t system = kernel::R32(SYM_UISystem_pointer);
    int screen = system ? int16_t(kernel::R16(system + kUiVirtualWidth)) : 0;
    if (!element || element == kernel::R32(game_box + kWidgetElement) || screen <= 0) return false;
    Write32(widget + kWidgetNameCrc, kCrcBox);
    KeepShown(widget);
    MirrorX(widget, kBoxWidth, screen);
    g_box = {hud, widget, element};
    g_extras = MakeExtras(ctx, hud, Read32(element + kElementArea));
    kernel::CallGuest(ctx, kSetText, {kernel::R32(game_box + kWidgetElement), kCrcDesc, Text("")});
    return true;
}

void Show(PPCContext& ctx, uint32_t hud, const xam::Achievement& a) {
    if (!EnsureBox(ctx, hud)) return;
    uint32_t element = g_box.element;
    uint32_t sprite = a.image ? fcip::achievement_icons::Sprite(ctx, Read32(hud + kHudSprites), a.image) : 0;
    if (g_extras.icon) {
        kernel::CallGuest(ctx, kSetPicture, {element, kCrcItem, sprite});
        SetVisible(g_extras.icon, sprite != 0);
        FadeIcon();
    }
    if (g_extras.desc) {
        kernel::CallGuest(ctx, kSetText, {element, kCrcDesc, Text(a.description.c_str())});
        SetVisible(g_extras.desc, true);
    }
    std::string title = "Achievement unlocked: " + a.name;
    if (!g_extras.desc) title += "\n" + a.description;
    uint32_t text = View(title.c_str());  // after Text(): they share one buffer
    if (!text) return;
    uint32_t game_box = kernel::R32(hud + kHudHelpBox);
    kernel::W32(hud + kHudHelpBox, g_box.widget);
    kernel::CallGuest(ctx, kHudHelp, {hud, text, 1});
    kernel::W32(hud + kHudHelpBox, game_box);
    if (!g_showing) g_game_box_visible = Read8(game_box + kWidgetVisible) & 0x80;
    HideIdleGameBox(hud, true);
    g_showing = true;
}

}  // namespace

void fcip::achievement_toast::Tick(PPCContext& ctx) {
    for (uint16_t id : xam::TakeUnlocked()) g_queue.push_back(id);
    auto now = Clock::now();
    uint32_t game = kernel::R32(SYM_Game_pointer);
    UpdateBox(ctx, game, now);
    if (g_queue.empty() || now - g_shown < kGap || InMenu()) return;
    uint32_t hud = game ? Hud(ctx, game) : 0;
    if (!hud) return;
    std::vector<xam::Achievement> list = xam::Achievements();
    if (const xam::Achievement* a = Find(list, g_queue.front())) Show(ctx, hud, *a);
    g_queue.pop_front();
    g_shown = now;
}

namespace {
const bool g_registered = (fcip::g_play_tick = fcip::achievement_toast::Tick, true);
}  // namespace
