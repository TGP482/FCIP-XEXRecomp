// default.xex: the settings entries in the User page and the pause menu's OPTIONS, the pages they open inside
// CONTROLLER OPTIONS, and the main menu watermark
#include <iterator>

#include "common/menu.h"
#include "common/widgets.h"
#include "core/log.h"
#include "kernel/kernel.h"

using namespace fcip;
using namespace fcip::ui;

namespace {

constexpr uint32_t kCrcEditProfile = 0x3C5E4151u;
constexpr uint32_t kCrcVideo = 0xC8040339u;  // GAME OPTIONS, the template for every clone
constexpr uint32_t kCrcControlSetup = 0x7765E2D2u;

// primary vtables of the two menu classes
constexpr uint32_t kVtProfile = 0x8205CBA8u;  // CryProfile, P_EditProfile is pages[1]
constexpr uint32_t kVtInGame = 0x82059CA0u;   // CryInGame, P_EditProfile is pages[4]
constexpr uint32_t kProfilePages = 0x50u;
constexpr uint32_t kProfileHelp = 0x1F4u;
constexpr uint32_t kInGameHelp = 0x10Cu;

uint32_t ProfilePage(uint32_t self, uint32_t index) {
    uint32_t page = Read32(Read32(self + kProfilePages) + index * 4);
    return page && Read32(page + kPageNameCrc) == kCrcEditProfile ? page : 0;
}

// the hidden multiplayer slot comes back the way GAME OPTIONS sits, rather than guessing the bits the hide cleared
void ShowEntry(uint32_t self) {
    uint32_t page = ProfilePage(self, 1);
    uint32_t widget = FindWidget(page, kCrcMultiplayerSetup);
    uint32_t sibling = FindWidget(page, kCrcVideo);
    if (!widget || !sibling || !AddrOk(Read32(widget + kWidgetElement))) return;
    Write8(widget + kWidgetVisible, Read8(widget + kWidgetVisible) | (Read8(sibling + kWidgetVisible) & 0x80));
    Write8(widget + kWidgetEnabled, Read8(widget + kWidgetEnabled) | Read8(sibling + kWidgetEnabled));
}

// the pause menu's copy of P_EditProfile has no spare slot, so it gets a button cloned from GAME OPTIONS
void AddEntriesFor(PPCContext& ctx, uint32_t self) {
    if (Read32(self) != kVtInGame) return;
    uint32_t page = ProfilePage(self, 4);
    AddEntries(ctx, page, FindWidget(page, kCrcVideo), kCrcVideo, kCrcControlSetup, {kCrcRecompSettings});
}

void SetHelp(PPCContext& ctx, uint32_t self, uint32_t help_offset, uint32_t sender) {
    uint32_t crc = Read32(sender + kWidgetNameCrc);
    if (PageFor(crc) >= 0) SetCaption(ctx, Read32(self + help_offset), HelpFor(crc));
}

// Native pages: CONTROLLER OPTIONS (P_Controller) hosts our rows. Its list has L_Name and L_Value columns and scrolls;
// Left and Right step a value, A steps it.

constexpr uint32_t kCrcController = 0x4CCDB8BDu;         // P_Controller
constexpr uint32_t kCrcControllerOptions = 0xA3EDB04Fu;  // LST_ControllerOptions
constexpr uint32_t kCrcControllerMap = 0x434B3537u;      // B_ControllerMap
constexpr uint32_t kCrcTitle = 0x06326AC4u;              // B_TitleButton
constexpr uint32_t kControllerHelp = 0xACu;              // A_Help element
// the list element, constructor 0x8246F288
constexpr uint32_t kVtList = 0x82078A40u;
constexpr uint32_t kListItems = 0x88u;
constexpr uint32_t kListItemBytes = 0x40u;
constexpr uint32_t kItemSkip = 0x3Eu;      // the keys step over it
constexpr uint32_t kListRows = 0xC2u;      // visible rows, 5 in the layout
constexpr uint32_t kListSelected = 0xC4u;  // s16
// 22 unit rows from y 125 end at 279, clear of the help frame at 299 once CONTROLLER SETUP is hidden
constexpr uint8_t kNativeRows = 7;

// the list's functions
constexpr uint32_t kListSetCell = 0x8246F6C8u;  // (list, row, column, const std::wstring*), copies the text
constexpr uint32_t kListRefresh = 0x8246FAB8u;  // (list)
constexpr uint32_t kListClear = 0x82471F18u;    // (list), keeps the columns
constexpr uint32_t kListAddRow = 0x82471CA0u;   // (list, text, 0)
constexpr uint32_t kListUpdateRow = 0x82472248u;  // (list, row)
constexpr uint32_t kListSelect = 0x82471970u;   // (list, row, 1, 1)

constexpr uint32_t kSoundSystem = 0x82912824u;
constexpr uint32_t kSoundPlay = 0x823BAD40u;  // (system, sound)
constexpr uint32_t kSoundChange = 0x2E60002u;

// menu key codes
constexpr uint32_t kKeyEnter = 0x0Du;
constexpr uint32_t kKeyLeft = 0x25u;
constexpr uint32_t kKeyRight = 0x27u;
constexpr uint32_t kKeyA = 0x2Fu;

struct Native {
    int mode = -1;  // the panel page shown, -1 while the stock page is up
    uint32_t obj = 0;
    uint32_t page = 0;
    uint32_t widget = 0;
    uint32_t list = 0;
};

// what Customise changed on a page; the layout keeps its pages across menu instances, so each keeps its record until
// it is put back
struct Saved {
    uint32_t page = 0;
    uint32_t widget = 0;
    uint32_t list = 0;
    uint32_t map = 0;
    uint32_t links[4] = {};
    uint8_t rows = 0;
    uint8_t map_visible = 0;
    uint8_t map_enabled = 0;
};

// guest menu thread only
Native g_native;
Saved g_saved[4];
int g_pending = -1;

const rows::Row& NativeRow(int row) { return rows::Page(g_native.mode)[size_t(row)]; }
int NativeCount() { return int(rows::Page(g_native.mode).size()); }

void SetCell(PPCContext& ctx, int row, int column, const char* text) {
    if (uint32_t view = View(text)) kernel::CallGuest(ctx, kListSetCell, {g_native.list, uint32_t(row), uint32_t(column), view});
}

void RefreshValues(PPCContext& ctx) {
    for (int i = 0; i < NativeCount(); i++) {
        if (NativeRow(i).kind == rows::Kind::Value) SetCell(ctx, i, 1, rows::Text(NativeRow(i)).c_str());
    }
}

int Selected() {
    int row = ReadS16(g_native.list + kListSelected);
    return row >= 0 && row < NativeCount() ? row : -1;
}

void NativeHelp(PPCContext& ctx) {
    int row = Selected();
    if (row < 0 || !NativeRow(row).help) return;
    SetCaption(ctx, Read32(g_native.obj + kControllerHelp), NativeRow(row).help);
}

void NativeTitle(PPCContext& ctx) {
    uint32_t title = FindWidget(g_native.page, kCrcTitle);
    SetCaption(ctx, Read32(title + kWidgetElement), CaptionFor(g_native.mode ? kCrcMouseKeyboard : kCrcRecompSettings));
}

// puts the page back the way the layout had it; the stock open rebuilds its own rows
void Restore(PPCContext& ctx, uint32_t page) {
    for (Saved& saved : g_saved) {
        if (!page || saved.page != page) continue;
        const Saved was = saved;
        saved = {};
        bool same = Read32(was.widget + kWidgetNameCrc) == kCrcControllerOptions &&
                    Read32(was.widget + kWidgetElement) == was.list && Read32(was.list) == kVtList;
        if (!same) return;
        for (uint32_t i = 0; i < 4; i++) Write32(was.widget + kWidgetUp + i * 4, was.links[i]);
        Write8(was.list + kListRows, was.rows);
        kernel::CallGuest(ctx, kListRefresh, {was.list});
        if (was.map) {
            Write8(was.map + kWidgetVisible, was.map_visible);
            Write8(was.map + kWidgetEnabled, was.map_enabled);
        }
    }
}

uint32_t ControllerPage(uint32_t obj) { return Read32(Read32(obj + kProfilePages)); }

Saved& SaveControls(uint32_t page, uint32_t widget, uint32_t list) {
    // a record left by a page that died without a back only costs its slot
    Saved* slot = &g_saved[0];
    for (Saved& saved : g_saved) {
        if (!saved.page) slot = &saved;
    }
    Saved& saved = *slot;
    saved = {page, widget, list, FindWidget(page, kCrcControllerMap)};
    for (uint32_t i = 0; i < 4; i++) saved.links[i] = Read32(widget + kWidgetUp + i * 4);
    saved.rows = Read8(list + kListRows);
    saved.map_visible = Read8(saved.map + kWidgetVisible);
    saved.map_enabled = Read8(saved.map + kWidgetEnabled);
    return saved;
}

void FillRows(PPCContext& ctx, uint32_t list) {
    kernel::CallGuest(ctx, kListClear, {list});
    for (int i = 0; i < NativeCount(); i++) {
        kernel::CallGuest(ctx, kListAddRow, {list, Text(""), 0});
        SetCell(ctx, i, 0, NativeRow(i).label);
        if (NativeRow(i).kind != rows::Kind::Heading) continue;
        Write8(Read32(list + kListItems) + uint32_t(i) * kListItemBytes + kItemSkip, 1);
        kernel::CallGuest(ctx, kListUpdateRow, {list, uint32_t(i)});
    }
    RefreshValues(ctx);
    kernel::CallGuest(ctx, kListRefresh, {list});
    kernel::CallGuest(ctx, kListSelect, {list, 1, 1, 1});  // row 0 is a heading
}

// after the stock open: our rows replace the stock options
void Customise(PPCContext& ctx, uint32_t obj, int mode) {
    uint32_t page = ControllerPage(obj);
    uint32_t widget = FindWidget(page, kCrcControllerOptions);
    uint32_t list = Read32(widget + kWidgetElement);
    if (Read32(page + kPageNameCrc) != kCrcController || !widget || Read32(list) != kVtList || rows::Page(mode).empty()) {
        LOGE("options menu: controller page %08X not as expected, stock page kept", page);
        return;
    }
    g_native = {mode, obj, page, widget, list};
    Saved& saved = SaveControls(page, widget, list);
    // Left and Right step values, so the list keeps the focus
    for (uint32_t i = 0; i < 4; i++) Write32(widget + kWidgetUp + i * 4, 0);
    if (saved.map) {
        Write8(saved.map + kWidgetVisible, saved.map_visible & 0x7F);
        Write8(saved.map + kWidgetEnabled, saved.map_enabled & ~0x40);
    }
    Write8(list + kListRows, kNativeRows);
    FillRows(ctx, list);
    NativeTitle(ctx);
    NativeHelp(ctx);
}

void NativeKey(PPCContext& ctx, uint32_t code) {
    int row = Selected();
    if (row < 0) return;
    const rows::Row& r = NativeRow(row);
    bool accept = code == kKeyA || code == kKeyEnter;
    int step = code == kKeyLeft ? -1 : (code == kKeyRight || accept) ? 1 : 0;
    if (!step) return;
    if (r.kind == rows::Kind::Value) rows::Text(r, step);
    else if (r.kind == rows::Kind::Defaults && accept) rows::Defaults(g_native.mode);
    else return;
    RefreshValues(ctx);
    kernel::CallGuest(ctx, kSoundPlay, {Read32(kSoundSystem), kSoundChange});
}

// swaps the sender for CONTROLLER SETUP's button so the stock press opens the controller page, which Customise then
// takes over
void OpenNative(PPCContext& ctx, uint8_t* base, uint32_t page, int mode, PPCFunc* press) {
    uint32_t control = FindWidget(page, kCrcControlSetup);
    if (!control) {
        OpenMenuPage(mode);
        return;
    }
    g_pending = mode;
    ctx.r[4] = control;
    press(ctx, base);
    g_pending = -1;
}

// Watermark: B_Text mirrors "{A} SELECT" at the bottom left of P_Main; the prompt setter empties it on every call,
// so this follows each one

constexpr uint32_t kCrcMainPage = 0x39514C08u;    // P_Main
constexpr uint32_t kCrcHeader = 0x8D948810u;      // A_Header
constexpr uint32_t kCrcNavigation = 0xDEB8BA8Bu;  // A_Navigation
constexpr uint32_t kCrcBackText = 0xF3EC853Bu;    // B_Text, empty on P_Main
constexpr uint32_t kVtTextElement = 0x82078CC8u;  // element type 8
constexpr uint32_t kElementArea = 0x2Cu;
constexpr uint32_t kElementAreaShown = 0x30u;
constexpr uint32_t kTextSetString = 0x821557A8u;  // (text element, UTF-16)
constexpr const char* kWatermark = "XEX Recomp V1";

void SetWatermark(PPCContext& ctx, uint32_t page) {
    if (Read32(page + kPageNameCrc) != kCrcMainPage) return;
    uint32_t header = Read32(FindWidget(page, kCrcHeader) + kWidgetElement);
    uint32_t nav = Read32(FindWidget(Read32(header + kElementArea), kCrcNavigation) + kWidgetElement);
    if (!Read32(nav + kElementAreaShown)) return;
    uint32_t text = Read32(FindWidget(Read32(nav + kElementArea), kCrcBackText) + kWidgetElement);
    if (Read32(text) != kVtTextElement) return;
    if (uint32_t guest = Text(kWatermark)) kernel::CallGuest(ctx, kTextSetString, {text, guest});
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_8216FA60);
PPC_EXTERN_FUNC(__imp__sub_821E31D0);
PPC_EXTERN_FUNC(__imp__sub_8216F570);
PPC_EXTERN_FUNC(__imp__sub_821E4648);
PPC_EXTERN_FUNC(__imp__sub_821E4210);
PPC_EXTERN_FUNC(__imp__sub_821AFD28);
PPC_EXTERN_FUNC(__imp__sub_821AF940);
PPC_EXTERN_FUNC(__imp__sub_821D42E8);
PPC_EXTERN_FUNC(__imp__sub_821D6100);
PPC_EXTERN_FUNC(__imp__sub_821D63D0);
PPC_EXTERN_FUNC(__imp__sub_821D6200);
PPC_EXTERN_FUNC(__imp__sub_821D65F0);
PPC_EXTERN_FUNC(__imp__sub_821BBA70);

// base menu init: resolves the pages, attaches listeners and widgets; the pages exist from here on
PPC_FUNC(sub_8216FA60) {
    uint32_t self = uint32_t(ctx.r[3]);
    __imp__sub_8216FA60(ctx, base);
    if (uint32_t(ctx.r[3]) == 1) AddEntriesFor(ctx, self);
}

// the User page hides its multiplayer slot in this build: it becomes RECOMP SETTINGS
PPC_FUNC(sub_821E31D0) {
    uint32_t self = uint32_t(ctx.r[3]);
    __imp__sub_821E31D0(ctx, base);
    ShowEntry(self);
}

// captions for our entries, after the page is localised
PPC_FUNC(sub_8216F570) {
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_8216F570(ctx, base);
    if (Read32(page + kPageNameCrc) == kCrcEditProfile) SetEntryCaptions(ctx, page);
}

// User page help text
PPC_FUNC(sub_821E4648) {
    uint32_t self = uint32_t(ctx.r[3]);
    uint32_t sender = uint32_t(ctx.r[4]);
    __imp__sub_821E4648(ctx, base);
    SetHelp(ctx, self, kProfileHelp, sender);
}

// User page press: our entries open their page instead of the native action
PPC_FUNC(sub_821E4210) {
    int page = PageFor(Read32(uint32_t(ctx.r[4]) + kWidgetNameCrc));
    if (page < 0) {
        __imp__sub_821E4210(ctx, base);
        return;
    }
    OpenNative(ctx, base, ProfilePage(uint32_t(ctx.r[3]), 1), page, __imp__sub_821E4210);
}

// pause menu OPTIONS help text
PPC_FUNC(sub_821AFD28) {
    uint32_t self = uint32_t(ctx.r[3]);
    uint32_t sender = uint32_t(ctx.r[4]);
    __imp__sub_821AFD28(ctx, base);
    SetHelp(ctx, self, kInGameHelp, sender);
}

// pause menu OPTIONS press
PPC_FUNC(sub_821AF940) {
    int page = PageFor(Read32(uint32_t(ctx.r[4]) + kWidgetNameCrc));
    if (page < 0) {
        __imp__sub_821AF940(ctx, base);
        return;
    }
    OpenNative(ctx, base, ProfilePage(uint32_t(ctx.r[3]), 4), page, __imp__sub_821AF940);
}

// CONTROLLER OPTIONS open (vtable +0x24), r3 the page object
PPC_FUNC(sub_821D42E8) {
    uint32_t obj = uint32_t(ctx.r[3]);
    int mode = g_pending;
    g_pending = -1;
    g_native.mode = -1;
    Restore(ctx, ControllerPage(obj));
    __imp__sub_821D42E8(ctx, base);
    if (mode >= 0) Customise(ctx, obj, mode);
}

// CONTROLLER OPTIONS page activated, r4 the page: sets the title
PPC_FUNC(sub_821D6100) {
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_821D6100(ctx, base);
    if (g_native.mode >= 0 && page == g_native.page) NativeTitle(ctx);
}

// CONTROLLER OPTIONS focus, r4 the widget: our rows carry no option, so the help is ours
PPC_FUNC(sub_821D63D0) {
    uint32_t sender = uint32_t(ctx.r[4]);
    __imp__sub_821D63D0(ctx, base);
    if (g_native.mode >= 0 && sender == g_native.widget) NativeHelp(ctx);
}

// CONTROLLER OPTIONS key up, r4 the widget, r5 the event; the stock A indexes its options by our row, so our list
// never reaches it
PPC_FUNC(sub_821D6200) {
    if (g_native.mode < 0 || uint32_t(ctx.r[4]) != g_native.widget) {
        __imp__sub_821D6200(ctx, base);
        return;
    }
    NativeKey(ctx, Read32(uint32_t(ctx.r[5])));
    ctx.r[3] = 0;
}

// CONTROLLER OPTIONS back: the page is put back once it is closed, so nothing flickers
PPC_FUNC(sub_821D65F0) {
    bool ours = g_native.mode >= 0;
    g_native.mode = -1;
    __imp__sub_821D65F0(ctx, base);
    if (ours) Restore(ctx, g_native.page);
}

// help bar prompt setter, r4 the page
PPC_FUNC(sub_821BBA70) {
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_821BBA70(ctx, base);
    SetWatermark(ctx, page);
}
