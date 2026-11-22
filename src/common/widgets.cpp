#include "common/widgets.h"

#include <algorithm>
#include <mutex>

#include "common/menu.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fcip::ui {

namespace {

constexpr uint32_t kGuestLow = 0x00010000u;
constexpr uint32_t kGuestHigh = 0xC0000000u;
// a constructed object's vtable points into the image
constexpr uint32_t kImageLow = 0x82000000u;
constexpr uint32_t kImageHigh = 0x82A00000u;

// a container with more children than this is no menu page
constexpr uint32_t kMaxChildBytes = 0x4000u;
constexpr uint32_t kMaxListeners = 16u;
constexpr uint32_t kMaxKeyframes = 64u;

constexpr uint32_t kWidgetTrack = 0x0Cu;      // keyframes at [+0x04, +0x08), state at key+0x04
constexpr uint32_t kWidgetListeners = 0x30u;  // [+0x30, +0x34), refcount at listener+0x04
constexpr uint32_t kElementTransform = 0x04u;
constexpr uint32_t kTransformX = 0x10u;  // s16
constexpr uint32_t kTransformY = 0x12u;
constexpr uint32_t kVisitorBytes = 0x28u;

constexpr const char* kRecompCaption = "RECOMP SETTINGS";
constexpr const char* kRecompHelp = "Display and graphics settings";
constexpr const char* kInputCaption = "MOUSE AND KEYBOARD";
constexpr const char* kInputHelp = "Mouse settings and keyboard bindings";
constexpr const char* kControllerCaption = "CONTROLLER";
constexpr const char* kControllerHelp = "Controller sensitivity and gyro settings";
constexpr const char* kExitCaption = "EXIT";
constexpr const char* kExitHelp = "Close the game and return to the desktop";
constexpr const char* kCustomCaption = "CUSTOM CAMPAIGN";
constexpr const char* kCustomHelp = "Play the custom levels in the levels\\play folder in order";
constexpr const char* kOnlineCaption = "ONLINE JOIN IP";
constexpr const char* kStatsCaption = "STATISTICS";
constexpr const char* kStatsHelp = "Your stats and leaderboards from system link matches, kept with your profile";
constexpr const char* kAchievementsCaption = "ACHIEVEMENTS";
constexpr const char* kAchievementsHelp = "The achievements your profile has earned";

// UTF-16 text, then a string view of it, then scratch for the copy visitor and a by reference argument
constexpr uint32_t kTextUnits = 0x180u;
constexpr uint32_t kViewBytes = 0x20u;
constexpr uint32_t kTextBlockBytes = kTextUnits * 2 + kViewBytes + 0x40u;

std::mutex g_text_lock;
uint32_t g_text = 0;
uint32_t g_view = 0;
uint32_t g_scratch = 0;

bool EnsureText() {
    std::lock_guard lock(g_text_lock);
    if (g_text) return true;
    uint32_t block = mem::AllocSystem(kTextBlockBytes);
    if (!block) return false;
    g_view = block + kTextUnits * 2;
    g_scratch = g_view + kViewBytes;
    g_text = block;
    return true;
}

// big endian UTF-16 (the setter measures it with wcslen); units written, the terminator included
uint32_t WriteUtf16(uint32_t addr, const char* text) {
    uint32_t n = 0;
    for (; text[n] && n + 1 < kTextUnits; n++) kernel::W16(addr + n * 2, uint8_t(text[n]));
    kernel::W16(addr + n * 2, 0);
    return n + 1;
}

void WriteS16(uint32_t addr, int value) {
    if (AddrOk(addr)) kernel::W16(addr, uint16_t(int16_t(std::clamp(value, -32768, 32767))));
}

bool InImage(uint32_t addr) { return addr >= kImageLow && addr < kImageHigh; }

bool ElementOk(uint32_t element) { return AddrOk(element) && InImage(Read32(element)); }

// a heap vector of u32 [first, last) with a sane count
bool RangeOk(uint32_t first, uint32_t last, uint32_t max_count) {
    return AddrOk(first) && last >= first && (last - first) % 4 == 0 && (last - first) / 4 <= max_count;
}

struct Pos {
    int x = 0;
    int y = 0;
};

uint32_t TransformOf(uint32_t widget) {
    uint32_t element = Read32(widget + kWidgetElement);
    if (!ElementOk(element)) return 0;
    uint32_t transform = Read32(element + kElementTransform);
    if (!AddrOk(transform) || Read32(transform) != SYM_UITransform_vtable) return 0;
    return transform;
}

// every keyframe state; false when the track is not the known layout
template <typename F>
bool ForEachKeyState(uint32_t widget, F&& fn) {
    uint32_t track = Read32(widget + kWidgetTrack);
    if (!track) return true;
    if (!AddrOk(track)) return false;
    uint32_t first = Read32(track + 0x04);
    uint32_t last = Read32(track + 0x08);
    if (!first) return last == 0;
    if (!RangeOk(first, last, kMaxKeyframes)) return false;
    for (uint32_t it = first; it < last; it += 4) {
        uint32_t key = Read32(it);
        uint32_t state = AddrOk(key) ? Read32(key + 0x04) : 0;
        if (!AddrOk(state) || Read32(state) != SYM_UITransform_vtable) return false;
        fn(state);
    }
    return true;
}

bool WidgetOk(uint32_t widget) {
    if (!AddrOk(widget)) return false;
    uint32_t vtable = Read32(widget);
    if (vtable != SYM_UIWidgetText_vtable && vtable != SYM_UIWidgetFocus_vtable) return false;
    if (!TransformOf(widget)) return false;
    return ForEachKeyState(widget, [](uint32_t) {});
}

// the resting position: the last keyframe when the widget animates in, else the transform
Pos RestPos(uint32_t widget) {
    uint32_t source = TransformOf(widget);
    ForEachKeyState(widget, [&](uint32_t state) { source = state; });
    return {ReadS16(source + kTransformX), ReadS16(source + kTransformY)};
}

void Shift(uint32_t state, Pos by) {
    WriteS16(state + kTransformX, ReadS16(state + kTransformX) + by.x);
    WriteS16(state + kTransformY, ReadS16(state + kTransformY) + by.y);
}

void ShiftWidget(uint32_t widget, Pos by) {
    Shift(TransformOf(widget), by);
    ForEachKeyState(widget, [&](uint32_t state) { Shift(state, by); });
}

// the menu class attaches its press and focus listeners before the clone exists, so the clone takes the template's
// with a reference each
void CopyListeners(PPCContext& ctx, uint32_t from, uint32_t to) {
    if (Read32(to + kWidgetListeners) != Read32(to + kWidgetListeners + 4)) return;
    uint32_t first = Read32(from + kWidgetListeners);
    uint32_t last = Read32(from + kWidgetListeners + 4);
    if (!first || !RangeOk(first, last, kMaxListeners)) return;
    for (uint32_t it = first; it < last; it += 4) {
        uint32_t listener = Read32(it);
        if (!AddrOk(listener) || !InImage(Read32(listener))) continue;
        Write32(listener + 4, Read32(listener + 4) + 1);
        Write32(g_scratch + kVisitorBytes, listener);
        kernel::CallGuest(ctx, SYM_UIWidget_AddListener, {to, g_scratch + kVisitorBytes});
    }
}

// the game's widget copy visitor, as the layout loader builds it on its stack
uint32_t CloneWidget(PPCContext& ctx, uint32_t page, uint32_t source) {
    Write32(g_scratch, SYM_UICloneVisitor_vtable);
    Write32(g_scratch + 0x04, page);
    Write32(g_scratch + 0x08, page);
    for (uint32_t off = 0x0C; off < kVisitorBytes; off += 4) Write32(g_scratch + off, 0);
    kernel::CallGuest(ctx, SYM_UIWidget_Clone, {g_scratch, source});
    uint32_t widget = Read32(g_scratch + 0x0C);
    return WidgetOk(widget) ? widget : 0;
}

// the container's child vector sits at +0x04
void PushChild(PPCContext& ctx, uint32_t page, uint32_t widget) {
    Write32(g_scratch + kVisitorBytes, widget);
    kernel::CallGuest(ctx, SYM_PtrVector_PushBack, {page + 0x04, g_scratch + kVisitorBytes});
}

// splices widget into the up and down chain under prev
void LinkBelow(uint32_t prev, uint32_t widget) {
    uint32_t below = Read32(prev + kWidgetDown);
    Write32(widget + kWidgetUp, prev);
    Write32(widget + kWidgetDown, below);
    Write32(prev + kWidgetDown, widget);
    if (AddrOk(below) && Read32(below + kWidgetUp) == prev) Write32(below + kWidgetUp, widget);
}

}  // namespace

bool AddrOk(uint32_t addr) { return addr >= kGuestLow && addr < kGuestHigh; }
uint32_t Read32(uint32_t addr) { return AddrOk(addr) ? kernel::R32(addr) : 0; }
int16_t ReadS16(uint32_t addr) { return AddrOk(addr) ? int16_t(kernel::R16(addr)) : 0; }
uint8_t Read8(uint32_t addr) { return AddrOk(addr) ? kernel::R8(addr) : 0; }

void Write32(uint32_t addr, uint32_t value) {
    if (AddrOk(addr)) kernel::W32(addr, value);
}

void Write8(uint32_t addr, uint8_t value) {
    if (AddrOk(addr)) kernel::W8(addr, value);
}

// the container's child array is [+0x08, +0x0C) of CUIWidget*
uint32_t FindWidget(uint32_t container, uint32_t name_crc) {
    if (!AddrOk(container)) return 0;
    uint32_t first = Read32(container + 0x08);
    uint32_t last = Read32(container + 0x0C);
    if (!first || last < first || last - first > kMaxChildBytes) return 0;
    for (uint32_t it = first; it < last; it += 4) {
        uint32_t widget = Read32(it);
        if (widget && Read32(widget + kWidgetNameCrc) == name_crc) return widget;
    }
    return 0;
}

uint32_t Text(const char* text) {
    if (!text || !EnsureText()) return 0;
    WriteUtf16(g_text, text);
    return g_text;
}

// a std::wstring: pointer +0x04, length +0x14, capacity +0x18, where 8 or more means the pointer is used
uint32_t View(const char* text) {
    if (!text || !EnsureText()) return 0;
    uint32_t length = WriteUtf16(g_text, text) - 1;
    Write32(g_view + 0x04, g_text);
    Write32(g_view + 0x14, length);
    Write32(g_view + 0x18, std::max(length, 8u));
    return g_view;
}

void SetCaption(PPCContext& ctx, uint32_t element, const char* text) {
    if (!ElementOk(element)) return;
    if (uint32_t guest = Text(text)) kernel::CallGuest(ctx, SYM_UIElement_SetCaption, {element, guest});
}

void HideWidget(uint32_t widget) {
    if (!WidgetOk(widget)) return;
    Write8(widget + kWidgetVisible, uint8_t(Read8(widget + kWidgetVisible) & ~0x80u));
    Write8(widget + kWidgetEnabled, uint8_t(Read8(widget + kWidgetEnabled) & ~0x40u));
    uint32_t up = Read32(widget + kWidgetUp);
    uint32_t down = Read32(widget + kWidgetDown);
    if (AddrOk(up) && Read32(up + kWidgetDown) == widget) Write32(up + kWidgetDown, down);
    if (AddrOk(down) && Read32(down + kWidgetUp) == widget) Write32(down + kWidgetUp, up);
}

void MoveWidget(uint32_t widget, uint32_t place) {
    if (!WidgetOk(widget) || !WidgetOk(place)) return;
    Pos from = RestPos(widget);
    Pos to = RestPos(place);
    ShiftWidget(widget, {to.x - from.x, to.y - from.y});
}

const char* CaptionFor(uint32_t crc) {
    if (crc == kCrcMultiplayerSetup || crc == kCrcRecompSettings) return kRecompCaption;
    if (crc == kCrcMouseKeyboard) return kInputCaption;
    if (crc == kCrcControllerSettings) return kControllerCaption;
    if (crc == kCrcExit) return kExitCaption;
    if (crc == kCrcCustomCampaign) return kCustomCaption;
    if (crc == kCrcOnline) return kOnlineCaption;
    if (crc == kCrcStatistics) return kStatsCaption;
    if (crc == kCrcAchievements) return kAchievementsCaption;
    return nullptr;
}

const char* HelpFor(uint32_t crc) {
    if (crc == kCrcMultiplayerSetup || crc == kCrcRecompSettings) return kRecompHelp;
    if (crc == kCrcMouseKeyboard) return kInputHelp;
    if (crc == kCrcControllerSettings) return kControllerHelp;
    if (crc == kCrcExit) return kExitHelp;
    if (crc == kCrcCustomCampaign) return kCustomHelp;
    if (crc == kCrcStatistics) return kStatsHelp;
    if (crc == kCrcAchievements) return kAchievementsHelp;
    return nullptr;
}

int PageFor(uint32_t crc) {
    if (crc == kCrcMultiplayerSetup || crc == kCrcRecompSettings) return kSettingsPage;
    if (crc == kCrcMouseKeyboard) return kKeyboardPage;
    if (crc == kCrcControllerSettings) return kControllerPage;
    if (crc == kCrcAchievements) return kAchievementsPage;
    return -1;
}

uint32_t EntryFor(int page) {
    if (page == kKeyboardPage) return kCrcMouseKeyboard;
    if (page == kControllerPage) return kCrcControllerSettings;
    if (page == kAchievementsPage) return kCrcAchievements;
    return kCrcRecompSettings;
}

void SetEntryCaptions(PPCContext& ctx, uint32_t page) {
    constexpr uint32_t kEntries[] = {kCrcMultiplayerSetup, kCrcRecompSettings, kCrcMouseKeyboard,
                                     kCrcControllerSettings, kCrcExit, kCrcCustomCampaign, kCrcOnline,
                                     kCrcStatistics, kCrcAchievements};
    for (uint32_t crc : kEntries) {
        if (uint32_t widget = FindWidget(page, crc)) SetCaption(ctx, Read32(widget + kWidgetElement), CaptionFor(crc));
    }
}

void AddEntries(PPCContext& ctx, uint32_t page, uint32_t last, uint32_t source_crc, uint32_t above_crc,
                std::initializer_list<uint32_t> crcs, uint32_t push) {
    static std::mutex lock;
    std::lock_guard guard(lock);
    if (!page || !EnsureText()) return;
    uint32_t source = FindWidget(page, source_crc);
    uint32_t above = FindWidget(page, above_crc);
    if (!WidgetOk(source) || !WidgetOk(above) || !WidgetOk(last) || (push && !WidgetOk(push))) {
        LOGE("options menu: page %08X template widgets not as expected, no entries added", page);
        return;
    }
    Pos src = RestPos(source);
    Pos row = {src.x - RestPos(above).x, src.y - RestPos(above).y};
    if (row.x == 0 && row.y == 0) {
        LOGE("options menu: page %08X rows share one position, no entries added", page);
        return;
    }
    Pos bottom = RestPos(last);
    uint32_t prev = last;
    int step = 0;
    for (uint32_t crc : crcs) {
        step++;
        uint32_t widget = FindWidget(page, crc);
        if (!widget) {
            widget = CloneWidget(ctx, page, source);
            if (!widget) {
                LOGE("options menu: cloning %08X failed on page %08X", source_crc, page);
                return;
            }
            Write32(widget + kWidgetNameCrc, crc);
            ShiftWidget(widget, {bottom.x - src.x + row.x * step, bottom.y - src.y + row.y * step});
            CopyListeners(ctx, source, widget);
            PushChild(ctx, page, widget);
            LinkBelow(prev, widget);
            SetCaption(ctx, Read32(widget + kWidgetElement), CaptionFor(crc));
            if (push) ShiftWidget(push, row);
        }
        prev = widget;
    }
}

}  // namespace fcip::ui
