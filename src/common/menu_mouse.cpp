// Mouse in the game's own menus: hover focuses, left click is A, right click is B, the wheel is up and down. Lists
// (LOAD A MAP) select the row under the cursor. Everything goes through the game's own focus, selection and key
// paths. The messages arrive on the window thread, the hook runs on the guest menu thread.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include "app/app.h"
#include "common/aspect.h"
#include "common/input.h"
#include "common/keys.h"
#include "common/menu.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// menu manager: vtable +4 is its per frame input update
constexpr uint32_t kMgrGame = 0x10u;     // the game's no menu flag set: no menu input
constexpr uint32_t kMgrActive = 0x14u;   // zero: no menu input
constexpr uint32_t kMgrUi = 0x18u;       // input page stack at +0xB8
constexpr uint32_t kMgrBlocked = 0x1Cu;  // nonzero: nothing is dispatched
constexpr uint32_t kMgrModal = 0x20u;    // container that takes input first
constexpr uint32_t kUiInputStack = 0xB8u;
constexpr uint32_t kInputPriority = 10u;
constexpr uint32_t kUiVirtualW = 0x58u;  // the layout's virtual size, s16
constexpr uint32_t kUiVirtualH = 0x5Au;

// container: children [+0x08, +0x0C), focus +0x20, drawn while +0x1E bit 0x40
constexpr uint32_t kPageFirst = 0x08u;
constexpr uint32_t kPageLast = 0x0Cu;
constexpr uint32_t kPageDrawn = 0x1Eu;
constexpr uint32_t kPageFocus = 0x20u;
constexpr uint32_t kWidgetElement = 0x08u;
constexpr uint32_t kWidgetVisible = 0x18u;  // bit 0x80
constexpr uint32_t kWidgetState = 0x4Cu;    // bit 0x40 enabled
constexpr uint32_t kVtFocus = 0x0Cu;
constexpr uint32_t kVtUnfocus = 0x10u;
// element: vtable +0x10 writes s16 {x0, x1, y0, y1}; key input needs +0x6C set and +0x70 negative or the event's pad
constexpr uint32_t kVtElementRect = 0x10u;
constexpr uint32_t kElementInput = 0x6Cu;
constexpr uint32_t kElementPad = 0x70u;
// list element: selected row s16 +0xC4, items 0x40 bytes in [+0x88, +0x8C) with +0x3E set on rows the keys skip,
// input on while +0xD0 bit 0x40, pad s32 +0xD8 (-1 any); its hit test answers a row, -1, -2 up arrow or -3 down
constexpr uint32_t kListFirst = 0x88u;
constexpr uint32_t kListLast = 0x8Cu;
constexpr uint32_t kListItem = 0x40u;
constexpr uint32_t kItemSkip = 0x3Eu;
constexpr uint32_t kListSelected = 0xC4u;
constexpr uint32_t kListFlags = 0xD0u;
constexpr uint32_t kListPad = 0xD8u;
constexpr int kRowUp = -2;
constexpr int kRowDown = -3;
// codes the manager builds from pad buttons
constexpr uint32_t kKeyA = 0x2Fu;
constexpr uint32_t kKeyB = 0x30u;
constexpr uint32_t kKeyUp = 0x26u;
constexpr uint32_t kKeyDown = 0x28u;

constexpr uint32_t kImageLow = 0x82000000u;
constexpr uint32_t kImageHigh = 0x82A00000u;
constexpr uint32_t kMaxChildren = 256u;
constexpr int kMaxWheelSteps = 5;

bool GuestOk(uint32_t addr) { return addr >= 0x10000u && addr < 0xC0000000u; }
uint32_t Read32(uint32_t addr) { return GuestOk(addr) ? R32(addr) : 0; }
int16_t ReadS16(uint32_t addr) { return GuestOk(addr) ? int16_t(R16(addr)) : 0; }
uint8_t Read8(uint32_t addr) { return GuestOk(addr) ? R8(addr) : 0; }
bool ObjectOk(uint32_t object) { return GuestOk(object) && Read32(object) >= kImageLow && Read32(object) < kImageHigh; }
bool IsList(uint32_t element) { return Read32(element) == SYM_UIList_vtable; }

struct Input {
    float u = 0.0f;  // cursor over the presented frame, 0 to 1
    float v = 0.0f;
    bool have_pos = false;
    bool moved = false;
    int left_down = 0;
    int left_up = 0;
    int right_up = 0;
    int wheel = 0;  // detents, positive is up
};

std::mutex g_lock;
Input g_input;

// the window to the presented frame, which is boxed to its aspect
void SetPos(HWND hwnd, intptr_t lp) {
    RECT r;
    GetClientRect(hwnd, &r);
    float w = float(r.right);
    float h = float(r.bottom);
    if (w <= 0.0f || h <= 0.0f) return;
    float a = fcip::DisplayAspect();
    float fw = std::min(w, h * a);
    float fh = fw / a;
    float u = (float(GET_X_LPARAM(lp)) - (w - fw) * 0.5f) / fw;
    float v = (float(GET_Y_LPARAM(lp)) - (h - fh) * 0.5f) / fh;
    if (!g_input.have_pos || u != g_input.u || v != g_input.v) g_input.moved = true;
    g_input.u = u;
    g_input.v = v;
    g_input.have_pos = true;
}

Input TakeInput() {
    std::lock_guard lock(g_lock);
    Input out = g_input;
    g_input.moved = false;
    g_input.left_down = g_input.left_up = g_input.right_up = 0;
    g_input.wheel = 0;
    return out;
}

// guest calls with a scratch block of their own, this thread only
class Guest {
public:
    explicit Guest(PPCContext& ctx) : ctx_(ctx) {}

    static uint32_t Scratch() {
        static const uint32_t block = mem::AllocSystem(0x100);
        return block;
    }

    uint32_t Call(uint32_t fn, std::initializer_list<uint32_t> args) { return uint32_t(CallGuest(ctx_, fn, args)); }

    // through the object's guest vtable, as the game calls it
    uint32_t Virtual(uint32_t object, uint32_t slot, uint32_t r4 = 0) {
        uint32_t target = Read32(Read32(object) + slot);
        if (target < kImageLow || target >= kImageHigh) return 0;
        return Call(target, {object, r4});
    }

private:
    PPCContext& ctx_;
};

// the container the manager's update feeds: the modal one, else the input stack's top page
uint32_t InputPage(Guest& guest, uint32_t mgr) {
    if (Read32(mgr) != SYM_MenuManager_vtable) return 0;
    uint32_t game = Read32(mgr + kMgrGame);
    if (!GuestOk(game) || Read8(game + SYM_Game_NoMenu) != 0) return 0;
    if (!Read32(mgr + kMgrActive) || Read32(mgr + kMgrBlocked)) return 0;
    uint32_t page = Read32(mgr + kMgrModal);
    if (!page) {
        uint32_t ui = Read32(mgr + kMgrUi);
        if (!GuestOk(ui)) return 0;
        page = guest.Call(SYM_UIInputStack_Top, {ui + kUiInputStack, 0, kInputPriority});
    }
    if (!ObjectOk(page) || !(Read8(page + kPageDrawn) & 0x40)) return 0;
    uint32_t first = Read32(page + kPageFirst);
    uint32_t last = Read32(page + kPageLast);
    if (!GuestOk(first) || last < first || (last - first) / 4 > kMaxChildren) return 0;
    return page;
}

// the cursor: its share across the frame and its place in the layout's virtual screen space
struct Cursor {
    float u = 0.0f;
    int width = 0;  // the layout's
    int x = 0;
    int y = 0;
};

// x in the virtual space of 2D held at anchor
int VirtualX(const Cursor& c, float anchor) {
    float k = fcip::Scale2D();
    return int((c.u - anchor * (1.0f - k)) / k * float(c.width));
}

bool ToVirtual(float u, float v, Cursor& c) {
    uint32_t ui = Read32(SYM_UISystem_pointer);
    int vw = ReadS16(ui + kUiVirtualW);
    int vh = ReadS16(ui + kUiVirtualH);
    if (!GuestOk(ui) || vw <= 0 || vh <= 0) return false;
    c.u = u;
    c.width = vw;
    // pages are drawn scaled across about the centre, parts held in place about their own anchor
    c.x = VirtualX(c, 0.5f);
    c.y = int(v * float(vh));
    return true;
}

bool Inside(int x, int y, int x0, int x1, int y0, int y1) {
    return x >= std::min(x0, x1) && x < std::max(x0, x1) && y >= std::min(y0, y1) && y < std::max(y0, y1);
}

// topmost visible, enabled, focusable child under the cursor; children draw in order with no container transform,
// so the rects are virtual screen space; hit_x is the cursor's x where the hit is drawn
uint32_t HitTest(Guest& guest, uint32_t page, const Cursor& c, int& hit_x) {
    uint32_t first = Read32(page + kPageFirst);
    uint32_t last = Read32(page + kPageLast);
    uint32_t rect = Guest::Scratch();
    uint32_t hit = 0;
    for (uint32_t it = first; it < last; it += 4) {
        uint32_t widget = Read32(it);
        if (!ObjectOk(widget)) continue;
        if (!(Read8(widget + kWidgetVisible) & 0x80) || !(Read8(widget + kWidgetState) & 0x40)) continue;
        uint32_t element = Read32(widget + kWidgetElement);
        if (!ObjectOk(element)) continue;
        if (!(guest.Call(SYM_UIWidget_CanFocus, {widget}) & 0xFF)) continue;
        W32(rect, 0);
        W32(rect + 4, 0);
        guest.Virtual(element, kVtElementRect, rect);
        int x0 = ReadS16(rect);
        int x1 = ReadS16(rect + 2);
        int y0 = ReadS16(rect + 4);
        int y1 = ReadS16(rect + 6);
        int x = VirtualX(c, fcip::WidgetAnchor(widget));
        if (x0 != x1 && y0 != y1 && Inside(x, c.y, x0, x1, y0, y1)) {
            hit = widget;
            hit_x = x;
        }
    }
    return hit;
}

// the list row under the point, or kRowUp, kRowDown, -1; the rects are fresh after HitTest
int ListRow(Guest& guest, uint32_t element, int x, int y) {
    uint32_t point = Guest::Scratch() + 0x20;  // s16 {x, y}
    W32(point, uint32_t(x & 0xFFFF) << 16 | uint32_t(y & 0xFFFF));
    int row = int16_t(guest.Call(SYM_UIList_HitTest, {element, point}));
    if (row < 0) return row;
    uint32_t first = Read32(element + kListFirst);
    uint32_t last = Read32(element + kListLast);
    if (!GuestOk(first) || last < first || uint32_t(row) >= (last - first) / kListItem) return -1;
    return Read8(first + uint32_t(row) * kListItem + kItemSkip) ? -1 : row;
}

// the previous or next row as the keys move it, then the refresh the key path does; false at either end, where the
// keys would hand the focus to another widget
bool StepList(Guest& guest, uint32_t widget, uint32_t element, bool down) {
    uint32_t moved = guest.Call(down ? SYM_UIList_Next : SYM_UIList_Prev, {element});
    if (!(moved & 0xFF)) return false;
    guest.Virtual(widget, kVtFocus);
    return true;
}

void SelectRow(Guest& guest, uint32_t widget, uint32_t element, int row) {
    if (row < 0 || ReadS16(element + kListSelected) == row) return;
    guest.Call(SYM_UIList_Select, {element, uint32_t(row), 0, 0});
    guest.Virtual(widget, kVtFocus);
}

// what a key move does: the old widget's vtable +0x10, the new one's +0xC (highlight, help, sound), then the page
// stores the new focus
void Focus(Guest& guest, uint32_t page, uint32_t widget) {
    uint32_t current = Read32(page + kPageFocus);
    if (widget == current) return;
    if (ObjectOk(current)) {
        // the key path ignores up and down while the focused element's input gate is closed
        uint32_t element = Read32(current + kWidgetElement);
        bool open = IsList(element) ? (Read8(element + kListFlags) & 0x40) != 0 : Read8(element + kElementInput) != 0;
        if (!open) return;
        guest.Virtual(current, kVtUnfocus);
    }
    guest.Virtual(widget, kVtFocus);
    guest.Call(SYM_UIPage_StoreFocus, {page});
}

// a pad press and release, as the manager dispatches them
void Press(Guest& guest, uint32_t page, uint32_t code) {
    uint32_t pad = 0;
    uint32_t focus = Read32(page + kPageFocus);
    if (ObjectOk(focus)) {
        uint32_t element = Read32(focus + kWidgetElement);
        int32_t bound = int32_t(Read32(element + (IsList(element) ? kListPad : kElementPad)));
        if (bound >= 0 && bound < 4) pad = uint32_t(bound);
    }
    uint32_t event = Guest::Scratch() + 0x10;
    for (uint32_t fn : {SYM_UIPage_KeyDown, SYM_UIPage_KeyUp}) {
        W32(event, code);
        W32(event + 4, 0);
        W32(event + 8, pad);
        W32(event + 12, 0);
        guest.Call(fn, {page, event});
    }
}

// guest thread only
uint32_t g_press_page = 0;
uint32_t g_press_widget = 0;
int g_press_row = -1;

void Release(Guest& guest, uint32_t page, uint32_t hit, uint32_t element, int row) {
    // A only when the release lands on the pressed widget and it holds the focus
    bool pressed = hit && hit == g_press_widget && page == g_press_page && Read32(page + kPageFocus) == hit;
    g_press_widget = 0;
    if (!pressed) return;
    if (!IsList(element)) Press(guest, page, kKeyA);
    else if (row == g_press_row && row >= 0) Press(guest, page, kKeyA);
    else if (row == g_press_row && (row == kRowUp || row == kRowDown)) StepList(guest, hit, element, row == kRowDown);
}

void Pointer(Guest& guest, uint32_t page, const Input& in) {
    Cursor c;
    if (!(in.moved || in.left_down || in.left_up) || !ToVirtual(in.u, in.v, c)) return;
    uint32_t focus = Read32(page + kPageFocus);
    bool list_focused = ObjectOk(focus) && IsList(Read32(focus + kWidgetElement));
    int hit_x = c.x;
    uint32_t hit = HitTest(guest, page, c, hit_x);
    uint32_t element = hit ? Read32(hit + kWidgetElement) : 0;
    int row = -1;
    if (hit && IsList(element)) {
        // a list is entered the stock way, then owns hover until a click elsewhere
        if (hit == focus) {
            row = ListRow(guest, element, hit_x, c.y);
            SelectRow(guest, hit, element, row);
        }
    } else if (hit && (!list_focused || in.left_down)) {
        Focus(guest, page, hit);
    }
    if (in.left_down) {
        g_press_page = page;
        g_press_widget = hit;
        g_press_row = row;
    }
    if (in.left_up) Release(guest, page, hit, element, row);
}

void Update(PPCContext& ctx, uint32_t mgr) {
    Input in = TakeInput();
    // Steam turns the pad into cursor motion and clicks, the pad already drives the menu
    if (fcip::MenuOpen() || fcip::MouseLookActive() || fcip::keys::PadRecent() || fcip::keys::Capturing() ||
        !in.have_pos || !Guest::Scratch()) {
        g_press_widget = 0;
        return;
    }
    if (!in.moved && !in.left_down && !in.left_up && !in.right_up && !in.wheel) return;
    fcip::keys::NoteKbm();
    Guest guest(ctx);
    uint32_t page = InputPage(guest, mgr);
    if (!page) {
        g_press_widget = 0;
        return;
    }
    Pointer(guest, page, in);
    uint32_t focus = Read32(page + kPageFocus);
    if (!focus) return;
    if (in.right_up) {
        Press(guest, page, kKeyB);
        return;
    }
    int steps = std::clamp(in.wheel, -kMaxWheelSteps, kMaxWheelSteps);
    uint32_t element = ObjectOk(focus) ? Read32(focus + kWidgetElement) : 0;
    for (int i = 0; i < std::abs(steps); i++) {
        // straight to the list, so the wheel never pushes the focus out at either end
        if (element && IsList(element)) {
            if (!StepList(guest, focus, element, steps < 0)) break;
        } else {
            Press(guest, page, steps > 0 ? kKeyUp : kKeyDown);
        }
    }
}

}  // namespace

namespace fcip {

void MenuMouseMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
    std::lock_guard lock(g_lock);
    switch (msg) {
        case WM_MOUSEMOVE: SetPos(static_cast<HWND>(hwnd), lp); break;
        case WM_LBUTTONDOWN:
            SetPos(static_cast<HWND>(hwnd), lp);
            // the menu behind never sees it
            if (fcip::SkippableMovie()) {
                fcip::PressStart();
                break;
            }
            g_input.left_down++;
            break;
        case WM_LBUTTONUP:
            SetPos(static_cast<HWND>(hwnd), lp);
            g_input.left_up++;
            break;
        case WM_RBUTTONUP:
            SetPos(static_cast<HWND>(hwnd), lp);
            g_input.right_up++;
            break;
        case WM_MOUSEWHEEL: g_input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; break;
    }
}

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(MenuManager_UpdateInput));

// the menu manager's per frame input update; the mouse runs after the pad has had its turn
PPC_FUNC(GUEST(MenuManager_UpdateInput)) {
    uint32_t mgr = uint32_t(ctx.r[3]);
    GUEST_ORIG(MenuManager_UpdateInput)(ctx, base);
    if (fcip::g_menu_tick) fcip::g_menu_tick(ctx, mgr);
    Update(ctx, mgr);
}
