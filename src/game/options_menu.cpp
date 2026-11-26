// default.xex: the settings entries in the User page and the pause menu's OPTIONS, the pages they open inside
// CONTROLLER OPTIONS, EXIT and CUSTOM CAMPAIGN on the main menu and the main menu watermark
#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "common/custom_levels.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/widgets.h"
#include "core/log.h"
#include "game/achievement_icons.h"
#include "game/achievement_rows.h"
#include "game/exit_menu.h"
#include "game/profiles.h"
#include "game/virtual_keyboard.h"
#include "cpu/dispatch.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using namespace fcip;
using namespace fcip::ui;

namespace {

constexpr uint32_t kCrcEditProfile = 0x3C5E4151u;
constexpr uint32_t kCrcVideo = 0xC8040339u;  // GAME OPTIONS, the template for every clone
constexpr uint32_t kCrcControlSetup = 0x7765E2D2u;
constexpr uint32_t kCrcResetDefault = 0x3C4AD511u;
constexpr uint32_t kCrcMainPage = 0x39514C08u;  // P_Main
constexpr uint32_t kCrcMultiplayerPage = 0xE0159190u;  // P_Multiplayer
constexpr uint32_t kCrcOptions = 0x188FC220u;
constexpr uint32_t kCrcCredits = 0x89ADE9D9u;
constexpr uint32_t kCrcHeader = 0x8D948810u;  // A_Header
constexpr uint32_t kCrcHelp = 0x46740CFEu;    // A_Help
constexpr uint32_t kCrcLowerCorner = 0xA1A022D5u;  // A_Header's corner line under the rows, which meets A_Help's frame
constexpr uint32_t kCrcBlackPanel = 0x8EAC7B38u;   // A_Header's A_MidBlack02, the black behind the rows
// its pictures ending on the corner line or under the help box, and the diagonal on the corner
constexpr uint32_t kPanelBottoms[] = {0x18112B15u, 0x81187AAFu, 0xC7D6C2B1u};
constexpr uint32_t kPanelCorner = 0x5ED4BAA2u;
constexpr uint32_t kElementArea = 0x2Cu;      // a holder element's area, its children at [+0x08, +0x0C)
constexpr uint32_t kElementRestyle = 0x48u;   // element vtable: (element, widget) after the widget's state bits change
constexpr int kProfileRow = 35;               // P_EditProfile's rows sit this far apart
// P_Main's rows top to bottom
constexpr uint32_t kMainRows[] = {
    0x17130B2Fu,  // B_StoryMode
    0x28A23D05u,  // B_ExpansionCampaign
    0x344E8201u,  // B_Multiplayer
    0x08D70D76u,  // B_MapEditor
    kCrcOptions, kCrcCredits,
    0x3C3BFDBBu,  // B_Cheats
};

// primary vtables of the two menu classes
constexpr uint32_t kVtProfile = 0x8205CBA8u;  // CryProfile, P_EditProfile is pages[1]
constexpr uint32_t kVtInGame = 0x82059CA0u;   // CryInGame, P_EditProfile is pages[4]
constexpr uint32_t kVtPreGame = 0x82059FD0u;  // CryPreGame, P_Main is pages[1]
constexpr uint32_t kProfilePages = 0x50u;
constexpr uint32_t kProfileHelp = 0x1F4u;
constexpr uint32_t kInGameHelp = 0x10Cu;

uint32_t ProfilePage(uint32_t self, uint32_t index) {
    uint32_t page = Read32(Read32(self + kProfilePages) + index * 4);
    return page && Read32(page + kPageNameCrc) == kCrcEditProfile ? page : 0;
}

uint32_t MainPage(uint32_t self) {
    uint32_t page = Read32(Read32(self + kProfilePages) + 4);
    return page && Read32(page + kPageNameCrc) == kCrcMainPage ? page : 0;
}

// the lowest of the rows the layout shows
template <size_t N>
uint32_t LastRow(uint32_t page, const uint32_t (&rows)[N]) {
    uint32_t last = 0;
    for (uint32_t crc : rows) {
        uint32_t widget = FindWidget(page, crc);
        if (Read8(widget + kWidgetVisible) & 0x80) last = widget;
    }
    return last;
}

// the hidden multiplayer slot comes back the way GAME OPTIONS sits, rather than guessing the bits the hide cleared;
// its element restyles as after the hide (821E325C), else it keeps the disabled look until focus moves
void ShowEntry(PPCContext& ctx, uint32_t self) {
    uint32_t page = ProfilePage(self, 1);
    uint32_t widget = FindWidget(page, kCrcMultiplayerSetup);
    uint32_t sibling = FindWidget(page, kCrcVideo);
    uint32_t element = Read32(widget + kWidgetElement);
    if (!widget || !sibling || !AddrOk(element)) return;
    Write8(widget + kWidgetVisible, Read8(widget + kWidgetVisible) | (Read8(sibling + kWidgetVisible) & 0x80));
    Write8(widget + kWidgetEnabled, Read8(widget + kWidgetEnabled) | Read8(sibling + kWidgetEnabled));
    uint32_t restyle = Read32(Read32(element) + kElementRestyle);
    if (restyle) kernel::CallGuest(ctx, restyle, {element, widget});
}

// the User page once its rows are added; A_Header's area is one for every page showing it, so its frame is lowered
// only while the User page shows
uint32_t g_frame_page = 0;
bool g_frame_lowered = false;

// the corner line under the rows and the help box it runs into dy lower, the black behind the rows down to them, so
// RESET DEFAULTS, pushed down by the added rows, stays above them; false when the frame is not as expected
bool MoveBottomFrame(uint32_t page, int dy) {
    uint32_t header = Read32(Read32(FindWidget(page, kCrcHeader) + kWidgetElement) + kElementArea);
    uint32_t corner = FindWidget(header, kCrcLowerCorner);
    uint32_t help = FindWidget(page, kCrcHelp);
    uint32_t panel = Read32(Read32(FindWidget(header, kCrcBlackPanel) + kWidgetElement) + kElementArea);
    if (!corner || !help || !panel) {
        LOGR("options menu: User page frame not found, left in place");
        return false;
    }
    LowerWidget(corner, dy);
    LowerWidget(help, dy);
    for (uint32_t crc : kPanelBottoms) MoveRectEdges(FindWidget(panel, crc), 0, dy);
    MoveRectEdges(FindWidget(panel, kPanelCorner), dy, dy);
    return true;
}

// the page on top: the frame lowers for the User page and goes back for any other page showing the header; pages
// without it, as message boxes, leave it
void UpdateBottomFrame(uint32_t top) {
    if (!g_frame_page || !FindWidget(top, kCrcHeader)) return;
    bool lower = top == g_frame_page;
    if (lower == g_frame_lowered) return;
    if (MoveBottomFrame(g_frame_page, lower ? kProfileRow : -kProfileRow)) g_frame_lowered = lower;
}

// buttons cloned from GAME OPTIONS: MOUSE AND KEYBOARD, CONTROLLER and ACHIEVEMENTS on the User page take RESET
// DEFAULTS' row, which moves to the bottom; the pause menu's copy of P_EditProfile has no spare slot, so it gets all four
void AddEntriesFor(PPCContext& ctx, uint32_t self) {
    uint32_t vtable = Read32(self);
    if (vtable == kVtProfile) {
        uint32_t page = ProfilePage(self, 1);
        bool added = FindWidget(page, kCrcMouseKeyboard) != 0;
        AddEntries(ctx, page, FindWidget(page, kCrcMultiplayerSetup), kCrcVideo, kCrcControlSetup,
                   {kCrcMouseKeyboard, kCrcControllerSettings, kCrcAchievements}, FindWidget(page, kCrcResetDefault));
        if (!added && FindWidget(page, kCrcMouseKeyboard)) g_frame_page = page;
    } else if (vtable == kVtInGame) {
        uint32_t page = ProfilePage(self, 4);
        AddEntries(ctx, page, FindWidget(page, kCrcVideo), kCrcVideo, kCrcControlSetup,
                   {kCrcRecompSettings, kCrcMouseKeyboard, kCrcControllerSettings, kCrcAchievements});
    } else if (vtable == kVtPreGame) {
        uint32_t page = MainPage(self);
        // CUSTOM CAMPAIGN only with custom levels to play
        if (FindCustomLevels().empty()) {
            AddEntries(ctx, page, LastRow(page, kMainRows), kCrcCredits, kCrcOptions, {kCrcExit});
        } else {
            AddEntries(ctx, page, LastRow(page, kMainRows), kCrcCredits, kCrcOptions, {kCrcCustomCampaign, kCrcExit});
        }
    }
}

void SetHelp(PPCContext& ctx, uint32_t self, uint32_t help_offset, uint32_t sender) {
    uint32_t crc = Read32(sender + kWidgetNameCrc);
    if (PageFor(crc) >= 0) SetCaption(ctx, Read32(self + help_offset), HelpFor(crc));
}

// Native pages: CONTROLLER OPTIONS (P_Controller) hosts our rows. Its list has L_Name and L_Value columns and scrolls;
// Left and Right step a value, A opens the list of a row's values (an on / off row flips) or rebinds a key. A page
// first lists its sections, A opens one and B goes back to them.

constexpr uint32_t kCrcController = 0x4CCDB8BDu;         // P_Controller
constexpr uint32_t kCrcControllerOptions = 0xA3EDB04Fu;  // LST_ControllerOptions
constexpr uint32_t kCrcControllerMap = 0x434B3537u;      // B_ControllerMap
constexpr uint32_t kCrcTitle = 0x06326AC4u;              // B_TitleButton
constexpr uint32_t kControllerHelp = 0xACu;              // A_Help element
// the list element, constructor 0x8246F288
constexpr uint32_t kVtList = 0x82078A40u;
constexpr uint32_t kListItems = 0x88u;
constexpr uint32_t kListItemBytes = 0x40u;
constexpr uint32_t kListRows = 0xC2u;      // visible rows, 5 in the layout
constexpr uint32_t kListSelected = 0xC4u;  // s16
// a list row: a colour its text and pictures are drawn in, used when the flag is set (824705C0)
constexpr uint32_t kItemColour = 0x1Cu;
constexpr uint32_t kItemColoured = 0x20u;
constexpr uint32_t kLockedColour = 0xFF7F7F7Fu;  // ABGR
// 22 unit rows from y 125 end at 279, clear of the help frame at 299 once CONTROLLER SETUP is hidden
constexpr uint8_t kNativeRows = 7;

// the list's functions
constexpr uint32_t kListSetCell = 0x8246F6C8u;  // (list, row, column, const std::wstring*), copies the text
constexpr uint32_t kListRefresh = 0x8246FAB8u;  // (list)
constexpr uint32_t kListClear = 0x82471F18u;    // (list), keeps the columns
constexpr uint32_t kListAddRow = 0x82471CA0u;   // (list, text, 0)
constexpr uint32_t kListSelect = 0x82471970u;   // (list, row, 1, 1)
constexpr uint32_t kListAddColumn = 0x8246F500u;  // (list, name crc)
constexpr uint32_t kTextSetString = 0x821557A8u;  // (text element, UTF-16)

constexpr uint32_t kSoundSystem = 0x82912824u;
constexpr uint32_t kSoundPlay = 0x823BAD40u;  // (system, sound)
constexpr uint32_t kSoundChange = 0x2E60002u;

// menu key codes
constexpr uint32_t kKeyEnter = 0x0Du;
constexpr uint32_t kKeyLeft = 0x25u;
constexpr uint32_t kKeyRight = 0x27u;
constexpr uint32_t kKeyA = 0x2Fu;
constexpr uint32_t kKeyB = 0x30u;

struct Native {
    int mode = -1;  // the panel page shown, -1 while the stock page is up
    uint32_t obj = 0;
    uint32_t page = 0;
    uint32_t widget = 0;
    uint32_t list = 0;
    int capture = -1;  // the row waiting for a key
    int section = -1;  // the heading shown, -1 for the list of them
    int icons = -1;    // the achievements' picture column
    std::vector<const rows::Row*> rows;  // per list row
};

constexpr const char* kCaptureHelp = "Press the key to bind, Escape cancels";

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
    uint32_t icon = 0;  // the picture cloned into the row template
    uint32_t name = 0;  // L_Name, moved right of it
};

// The achievements' pictures: the value list's rating picture cloned into our list's row template, at the left with
// L_Name moved right of it, as another column whose cells take a sprite; put back with the page. A sprite loads its
// texture by path into the menu's sprites, as the map list does (821684E0), from the pictures' own pack.
constexpr uint32_t kListTemplate = 0x3Cu;  // the row template area
constexpr uint32_t kListColumns = 0x98u;   // name crcs [+0x98, +0x9C), a column per cell
constexpr uint32_t kItemCells = 0x2Cu;     // [+0x2C, +0x30)
constexpr uint32_t kCellBytes = 0x28u;
constexpr uint32_t kCellColour = 0x20u;
constexpr uint32_t kCellColoured = 0x24u;
constexpr uint32_t kListSetSprite = 0x8246F7A0u;  // (list, row, column, sprite)
constexpr int16_t kIconRect[4] = {0, 20, 1, 21};  // left, right, top, bottom in the 22 unit row
constexpr int kNameShift = 24;
constexpr uint32_t kMenuSprites = 0x14u;

// The value list: P_PopupValues as the stock rows open it for an option with more than two values (0x821BB360),
// listing one of our rows' values; its key up (0x821BAEA0) is ours while it does

// the controller page's option helper: +0x08 L_Description, +0x10 P_PopupValues, +0x28 its LST_Values,
// +0x2C B_TitleButton
constexpr uint32_t kHelper = 0x68u;
constexpr uint32_t kHelperDescription = 0x08u;
constexpr uint32_t kHelperPopup = 0x10u;
constexpr uint32_t kHelperValues = 0x28u;
constexpr uint32_t kHelperTitle = 0x2Cu;
constexpr uint32_t kHelperRowIcon = 0x821BB968u;  // (helper, row, const u32* colour), the colour hides the rating
constexpr uint32_t kIconHidden = 0x00FFFFFFu;
constexpr uint32_t kCrcName = 0xAC4E3D8Au;        // L_Name
constexpr uint32_t kCrcRating = 0x23D9A6A4u;      // i_icon_rating
// a listener as the stock open builds it: +0x04 references, +0x08 enabled, +0x10 the helper
constexpr uint32_t kVtValuesListener = 0x8205A79Cu;
constexpr uint32_t kVtValuesListenerBase = 0x8205A7D4u;
constexpr uint32_t kVtSoundListener = 0x82056974u;
constexpr uint32_t kVtSoundListenerBase = 0x8205638Cu;
constexpr uint32_t kListenerBase = 0x0Cu;
constexpr uint32_t kListenerRefs = 0x04u;
constexpr uint32_t kListenerEnabled = 0x08u;
constexpr uint32_t kListenerHelper = 0x10u;
constexpr uint32_t kListenerBytes = 0x14u;
constexpr uint32_t kAddListener = 0x8246CCA0u;      // (page, listener**) to each widget, drops the passed reference
constexpr uint32_t kRemoveListeners = 0x8246CD68u;  // (page)
// the help bar: (bar, page, const std::string* buttons, const std::string* text, 1)
constexpr uint32_t kHelpBar = 0x8293A878u;
constexpr uint32_t kSetHelpBar = 0x821BBA70u;
constexpr uint32_t kPopupButtons = 0x8205A6A0u;  // "Y,Empty_Y,B,Default_B,A,Default_A"
constexpr uint32_t kPopupText = 0x82056B88u;     // " "
constexpr uint32_t kStringInit = 0x820C0DB8u;    // (std::string*, const char*)
constexpr uint32_t kStringFree = 0x820C0518u;    // (std::string*, 1, 0)
constexpr uint32_t kStringBytes = 0x1Cu;
// the UI manager: +0x340 its draw stack, whose +0x18 holds the input stack at +0xB8 and the drawn pages at +0x38
constexpr uint32_t kUiManager = 0x8291234Cu;
constexpr uint32_t kUiDraw = 0x340u;
constexpr uint32_t kDrawInput = 0x18u;
constexpr uint32_t kInputStack = 0xB8u;
constexpr uint32_t kDrawnPages = 0x38u;
constexpr uint32_t kPushDraw = 0x821303B0u;      // (draw, page, 1, 0, 0, 0)
constexpr uint32_t kPopDraw = 0x821304D8u;       // (draw, 0, 0, 0, 0)
constexpr uint32_t kInputTop = 0x8246B608u;      // (stack, 0, 0)
constexpr uint32_t kPushInput = 0x8246B158u;     // (stack, page, 1, 0)
constexpr uint32_t kPopInput = 0x8246B318u;      // (stack, 0, 0)
constexpr uint32_t kDefaultFocus = 0x8246BB58u;  // (page)

// the game's message box as RESET DEFAULTS on the User page opens it (0x821E44F8): (box, id, const std::wstring* title,
// text, buttons, priority, listener, two button texts, 0, 0, 0, -1); the listener's vtable +0 runs on OK, +4 on cancel
constexpr uint32_t kMessageBox = 0x8216B168u;
constexpr uint32_t kMenuUi = 0x10u;  // a menu object's UI, its message boxes at +0x0C
constexpr uint32_t kUiMessageBoxes = 0x0Cu;
constexpr uint32_t kOkCancel = 4u;
constexpr const char* kDefaultsQuestion = "Restore every setting on this page to its default?";
constexpr const char* kResetQuestion = "Lock every achievement this profile has earned?";
constexpr uint32_t kWideCapacity = 0x18u;
constexpr uint32_t kInlineCapacity = 7u;

// our block: the two listeners, the help bar's strings, the icon colour, the listener argument, the message box's
// listener with its three slot vtable after it and an empty std::wstring
constexpr uint32_t kBlockValuesListener = 0x00u;
constexpr uint32_t kBlockSoundListener = kListenerBytes;
constexpr uint32_t kBlockButtons = kListenerBytes * 2;
constexpr uint32_t kBlockText = kBlockButtons + kStringBytes;
constexpr uint32_t kBlockIcon = kBlockText + kStringBytes;
constexpr uint32_t kBlockArgument = kBlockIcon + 4;
constexpr uint32_t kBlockConfirm = kBlockArgument + 4;
constexpr uint32_t kBlockEmpty = kBlockConfirm + 16;
constexpr uint32_t kBlockBytes = kBlockEmpty + kStringBytes;

struct Popup {
    uint32_t block = 0;
    uint32_t helper = 0;
    int row = -1;  // our row whose values it lists
    int current = 0;
};

// guest menu thread only
Native g_native;
Saved g_saved[4];
int g_pending = -1;
Popup g_popup;

std::span<const rows::Row> PageRows(int mode) {
    return mode == kAchievementsPage ? achievement_rows::Rows() : rows::Page(mode);
}

const rows::Row& NativeRow(int row) { return *g_native.rows[size_t(row)]; }
int NativeCount() { return int(g_native.rows.size()); }

// our rows are still in the list
bool ListOk() {
    uint32_t first = Read32(g_native.list + kListItems);
    uint32_t last = Read32(g_native.list + kListItems + 4);
    return Read32(g_native.list) == kVtList && last >= first && (last - first) / kListItemBytes == uint32_t(NativeCount());
}

// a heading or RESTORE DEFAULTS, where a section ends
bool SectionEnd(const rows::Row& r) { return r.kind == rows::Kind::Heading || r.kind == rows::Kind::Defaults; }

// the rows under the section's heading, or the headings and RESTORE DEFAULTS
void ListRows() {
    std::span<const rows::Row> page = PageRows(g_native.mode);
    g_native.rows.clear();
    if (g_native.section < 0) {
        for (const rows::Row& r : page) {
            if (SectionEnd(r)) g_native.rows.push_back(&r);
        }
        return;
    }
    for (size_t i = size_t(g_native.section) + 1; i < page.size() && !SectionEnd(page[i]); i++) {
        if (!rows::PanelOnly(page[i])) g_native.rows.push_back(&page[i]);
    }
}

void SetCell(PPCContext& ctx, int row, int column, const char* text) {
    if (uint32_t view = View(text)) kernel::CallGuest(ctx, kListSetCell, {g_native.list, uint32_t(row), uint32_t(column), view});
}

void RefreshValues(PPCContext& ctx) {
    bool achievements = g_native.mode == kAchievementsPage;
    for (int i = 0; i < NativeCount(); i++) {
        const rows::Row& r = NativeRow(i);
        if (achievements) {
            SetCell(ctx, i, 1, achievement_rows::Value(r).c_str());
            continue;
        }
        if (r.kind != rows::Kind::Value && r.kind != rows::Kind::Bind) continue;
        std::string value = g_native.capture == i ? "PRESS A KEY" : rows::Text(r);
        SetCell(ctx, i, 1, value.c_str());
    }
}

int Selected() {
    int row = ReadS16(g_native.list + kListSelected);
    return row >= 0 && row < NativeCount() ? row : -1;
}

void NativeHelp(PPCContext& ctx) {
    int row = Selected();
    if (row < 0) return;
    const char* help = g_native.capture == row ? kCaptureHelp : NativeRow(row).help;
    if (help) SetCaption(ctx, Read32(g_native.obj + kControllerHelp), help);
}

void NativeTitle(PPCContext& ctx) {
    uint32_t title = FindWidget(g_native.page, kCrcTitle);
    const char* caption = CaptionFor(EntryFor(g_native.mode));
    if (g_native.section >= 0) caption = PageRows(g_native.mode)[size_t(g_native.section)].label;
    SetCaption(ctx, Read32(title + kWidgetElement), caption);
}

// the last of a guest vector of u32 [first, last) dropped when it is value
void PopIf(uint32_t vector_first, uint32_t value) {
    uint32_t last = Read32(vector_first + 4);
    if (last > Read32(vector_first) && Read32(last - 4) == value) Write32(vector_first + 4, last - 4);
}

void RemoveIconColumn(const Saved& was) {
    if (!was.icon) return;
    PopIf(Read32(was.list + kListTemplate) + 0x08, was.icon);
    PopIf(was.list + kListColumns, kCrcRating);
    ShiftRect(was.name, -kNameShift);
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
        RemoveIconColumn(was);
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

// the SPA picture's sprite, made once per menu
uint32_t SpriteFor(PPCContext& ctx, uint32_t image) {
    return achievement_icons::Sprite(ctx, Read32(g_native.obj + kMenuSprites), image);
}

int ColumnOf(uint32_t list, uint32_t crc) {
    uint32_t first = Read32(list + kListColumns);
    uint32_t last = Read32(list + kListColumns + 4);
    for (uint32_t it = first; it && it < last; it += 4) {
        if (Read32(it) == crc) return int((it - first) / 4);
    }
    return -1;
}

void AddIconColumn(PPCContext& ctx, Saved& saved) {
    uint32_t list = g_native.list;
    uint32_t row = Read32(list + kListTemplate);
    uint32_t values = Read32(g_native.obj + kHelper + kHelperValues);
    uint32_t rating = FindWidget(Read32(values + kListTemplate), kCrcRating);
    uint32_t name = FindWidget(row, kCrcName);
    if (!rating || !name || FindWidget(row, kCrcRating)) {
        LOGE("options menu: achievement pictures not added, row template %08X", row);
        return;
    }
    uint32_t icon = CloneChild(ctx, row, rating);
    if (!icon) return;
    SetRect(icon, kIconRect);
    ShiftRect(name, kNameShift);
    kernel::CallGuest(ctx, kListAddColumn, {list, kCrcRating});
    saved.icon = icon;
    saved.name = name;
    g_native.icons = ColumnOf(list, kCrcRating);
}

// a picture per achievement, none on the other rows
void SetIcons(PPCContext& ctx, uint32_t list) {
    for (int i = 0; g_native.icons >= 0 && i < NativeCount(); i++) {
        uint32_t image = achievement_rows::Image(NativeRow(i));
        uint32_t sprite = image ? SpriteFor(ctx, image) : 0;
        kernel::CallGuest(ctx, kListSetSprite, {list, uint32_t(i), uint32_t(g_native.icons), sprite});
        uint32_t cell = Read32(Read32(list + kListItems) + uint32_t(i) * kListItemBytes + kItemCells);
        cell += uint32_t(g_native.icons) * kCellBytes;
        Write32(cell + kCellColour, kIconHidden);
        Write8(cell + kCellColoured, sprite == 0);
    }
}

void FillRows(PPCContext& ctx, uint32_t list, int select) {
    kernel::CallGuest(ctx, kListClear, {list});
    for (int i = 0; i < NativeCount(); i++) {
        kernel::CallGuest(ctx, kListAddRow, {list, Text(""), 0});
        SetCell(ctx, i, 0, NativeRow(i).label);
        uint32_t item = Read32(list + kListItems) + uint32_t(i) * kListItemBytes;
        bool locked = achievement_rows::Locked(NativeRow(i));
        Write32(item + kItemColour, kLockedColour);
        Write8(item + kItemColoured, locked);
    }
    SetIcons(ctx, list);
    RefreshValues(ctx);
    kernel::CallGuest(ctx, kListRefresh, {list});
    kernel::CallGuest(ctx, kListSelect, {list, uint32_t(select), 1, 1});
}

// after the stock open: our rows replace the stock options
void Customise(PPCContext& ctx, uint32_t obj, int mode) {
    uint32_t page = ControllerPage(obj);
    uint32_t widget = FindWidget(page, kCrcControllerOptions);
    uint32_t list = Read32(widget + kWidgetElement);
    if (mode == kAchievementsPage) achievement_rows::Refresh();
    if (Read32(page + kPageNameCrc) != kCrcController || !widget || Read32(list) != kVtList || PageRows(mode).empty()) {
        LOGE("options menu: controller page %08X not as expected, stock page kept", page);
        return;
    }
    g_native = {mode, obj, page, widget, list};
    ListRows();
    Saved& saved = SaveControls(page, widget, list);
    // Left and Right step values, so the list keeps the focus
    for (uint32_t i = 0; i < 4; i++) Write32(widget + kWidgetUp + i * 4, 0);
    if (saved.map) {
        Write8(saved.map + kWidgetVisible, saved.map_visible & 0x7F);
        Write8(saved.map + kWidgetEnabled, saved.map_enabled & ~0x40);
    }
    Write8(list + kListRows, kNativeRows);
    if (mode == kAchievementsPage) AddIconColumn(ctx, saved);
    FillRows(ctx, list, 0);
    NativeTitle(ctx);
    NativeHelp(ctx);
}

// a section's rows, or with -1 the sections again with the one left selected
void ShowSection(PPCContext& ctx, int section) {
    std::span<const rows::Row> page = PageRows(g_native.mode);
    const rows::Row* left = g_native.section >= 0 ? &page[size_t(g_native.section)] : nullptr;
    g_native.section = section;
    ListRows();
    auto it = std::find(g_native.rows.begin(), g_native.rows.end(), left);
    int select = it == g_native.rows.end() ? 0 : int(it - g_native.rows.begin());
    FillRows(ctx, g_native.list, select);
    NativeTitle(ctx);
    NativeHelp(ctx);
}

// the message box's OK
void DefaultsConfirmed(PPCContext& ctx, uint8_t*) {
    if (g_native.mode < 0 || !ListOk()) return;
    if (g_native.mode == kAchievementsPage) {
        achievement_rows::Reset();
        ListRows();
        FillRows(ctx, g_native.list, NativeCount() - 1);
    } else {
        rows::Defaults(g_native.mode);
        RefreshValues(ctx);
    }
    kernel::CallGuest(ctx, kSoundPlay, {Read32(kSoundSystem), kSoundChange});
}

void Ignored(PPCContext&, uint8_t*) {}

// the listeners keep our reference, so the page dropping its own never frees them
bool EnsureBlock() {
    if (g_popup.block) return true;
    uint32_t block = mem::AllocSystem(kBlockBytes);
    if (!block) return false;
    uint32_t values = block + kBlockValuesListener;
    uint32_t sound = block + kBlockSoundListener;
    Write32(values, kVtValuesListener);
    Write32(values + kListenerBase, kVtValuesListenerBase);
    Write32(sound, kVtSoundListener);
    Write32(sound + kListenerBase, kVtSoundListenerBase);
    for (uint32_t listener : {values, sound}) {
        Write32(listener + kListenerRefs, 1);
        Write8(listener + kListenerEnabled, 1);
    }
    uint32_t confirm = block + kBlockConfirm;
    Write32(confirm, confirm + 4);
    Write32(confirm + 4, dispatch::HostThunk(DefaultsConfirmed));
    Write32(confirm + 8, dispatch::HostThunk(Ignored));
    Write32(confirm + 12, dispatch::HostThunk(Ignored));
    uint32_t empty = block + kBlockEmpty;
    for (uint32_t at = 0; at < kStringBytes; at += 4) Write32(empty + at, 0);
    Write32(empty + kWideCapacity, kInlineCapacity);
    g_popup.block = block;
    return true;
}

void AskDefaults(PPCContext& ctx) {
    uint32_t boxes = Read32(Read32(g_native.obj + kMenuUi) + kUiMessageBoxes);
    if (!boxes || !EnsureBlock()) return;
    uint32_t text = View(g_native.mode == kAchievementsPage ? kResetQuestion : kDefaultsQuestion);
    uint32_t empty = g_popup.block + kBlockEmpty;
    uint32_t listener = g_popup.block + kBlockConfirm;
    kernel::CallGuest(ctx, kMessageBox,
                      {boxes, 0, empty, text, kOkCancel, 0, listener, empty, empty, 0, 0, 0, 0xFFFFFFFFu});
}

uint32_t DrawStack() { return Read32(Read32(kUiManager) + kUiDraw); }
uint32_t InputStack() { return Read32(DrawStack() + kDrawInput) + kInputStack; }

void AddListener(PPCContext& ctx, uint32_t page, uint32_t listener) {
    uint32_t argument = g_popup.block + kBlockArgument;
    Write32(listener + kListenerRefs, Read32(listener + kListenerRefs) + 1);
    Write32(argument, listener);
    kernel::CallGuest(ctx, kAddListener, {page, argument});
}

void SetPopupHelpBar(PPCContext& ctx, uint32_t page) {
    uint32_t buttons = g_popup.block + kBlockButtons;
    uint32_t text = g_popup.block + kBlockText;
    kernel::CallGuest(ctx, kStringInit, {buttons, kPopupButtons});
    kernel::CallGuest(ctx, kStringInit, {text, kPopupText});
    kernel::CallGuest(ctx, kSetHelpBar, {Read32(kHelpBar), page, buttons, text, 1});
    kernel::CallGuest(ctx, kStringFree, {buttons, 1, 0});
    kernel::CallGuest(ctx, kStringFree, {text, 1, 0});
}

// as 0x821BAFF8 fills it for an option
void FillValues(PPCContext& ctx, uint32_t values, const std::vector<std::string>& choices) {
    kernel::CallGuest(ctx, kListAddColumn, {values, kCrcName});
    kernel::CallGuest(ctx, kListAddColumn, {values, kCrcRating});
    kernel::CallGuest(ctx, kListClear, {values});
    for (const std::string& choice : choices) {
        uint32_t row = uint32_t(kernel::CallGuest(ctx, kListAddRow, {values, Text(""), 0}));
        if (uint32_t view = View(choice.c_str())) kernel::CallGuest(ctx, kListSetCell, {values, row, 0, view});
    }
    uint32_t icon = g_popup.block + kBlockIcon;
    Write32(icon, kIconHidden);
    for (uint32_t row = 0; row < uint32_t(choices.size()); row++) {
        kernel::CallGuest(ctx, kHelperRowIcon, {g_popup.helper, row, icon});
    }
    // scrolled to, the long lists (sensitivity, field of view) start far down
    kernel::CallGuest(ctx, kListSelect, {values, uint32_t(g_popup.current), 0, 1});
    uint32_t description = Read32(g_popup.helper + kHelperDescription);
    const char* help = NativeRow(g_popup.row).help;
    if (description && help) kernel::CallGuest(ctx, kTextSetString, {description, Text(help)});
}

// as 0x821BB360 opens it
void OpenPopup(PPCContext& ctx, int row, const std::vector<std::string>& choices, int current) {
    uint32_t helper = g_native.obj + kHelper;
    uint32_t page = Read32(helper + kHelperPopup);
    uint32_t values = Read32(helper + kHelperValues);
    if (!page || Read32(values) != kVtList || !EnsureBlock()) {
        LOGE("options menu: value list %08X not as expected", page);
        return;
    }
    g_popup.helper = helper;
    g_popup.row = row;
    g_popup.current = current;
    Write32(g_popup.block + kBlockValuesListener + kListenerHelper, helper);
    AddListener(ctx, page, g_popup.block + kBlockValuesListener);
    AddListener(ctx, page, g_popup.block + kBlockSoundListener);
    SetPopupHelpBar(ctx, page);
    FillValues(ctx, values, choices);
    kernel::CallGuest(ctx, kPushDraw, {DrawStack(), page, 1, 0, 0, 0});
    uint32_t stack = InputStack();
    if (uint32_t(kernel::CallGuest(ctx, kInputTop, {stack, 0, 0})) != page) {
        kernel::CallGuest(ctx, kPushInput, {stack, page, 1, 0});
    }
    kernel::CallGuest(ctx, kDefaultFocus, {page});
    SetCaption(ctx, Read32(helper + kHelperTitle), "");
}

// step 0 is A, which opens a row's list, flips an on / off row and steps a row without a list; false when nothing
// changed
bool StepValue(PPCContext& ctx, int row, int step) {
    const rows::Row& r = NativeRow(row);
    int current = 0;
    std::vector<std::string> choices = step ? std::vector<std::string>() : rows::Choices(r, &current);
    // either may read (DEFAULT)
    bool flip = choices.size() == 2 && choices[0].starts_with("OFF") && choices[1].starts_with("ON");
    if (choices.size() > 1 && !flip) {
        OpenPopup(ctx, row, choices, current);
        return false;
    }
    rows::Text(r, step ? step : 1);
    return true;
}

void StartCapture(PPCContext& ctx, int row) {
    g_native.capture = row;
    keys::BeginCapture();
    SetCell(ctx, row, 1, "PRESS A KEY");
    NativeHelp(ctx);
}

void NativeKey(PPCContext& ctx, uint32_t code) {
    int row = Selected();
    if (row < 0 || g_native.capture >= 0) return;
    const rows::Row& r = NativeRow(row);
    bool accept = code == kKeyA || code == kKeyEnter;
    int step = 0;
    if (code == kKeyLeft) step = -1;
    if (code == kKeyRight) step = 1;
    if (!accept && !step) return;
    // only the list of sections can select a heading
    if (r.kind == rows::Kind::Heading) {
        if (accept) ShowSection(ctx, int(&r - PageRows(g_native.mode).data()));
        return;
    }
    if (r.kind == rows::Kind::Bind) {
        if (accept) StartCapture(ctx, row);
        return;
    }
    if (r.kind == rows::Kind::Defaults) {
        if (accept) AskDefaults(ctx);
        return;
    }
    if (r.kind != rows::Kind::Value || !StepValue(ctx, row, step)) return;
    RefreshValues(ctx);
    kernel::CallGuest(ctx, kSoundPlay, {Read32(kSoundSystem), kSoundChange});
}

void Leave() {
    g_native.mode = -1;
    g_native.section = -1;
    g_popup.row = -1;
    if (g_native.capture >= 0 && keys::Capturing()) keys::Captured();
    g_native.capture = -1;
}

// A picks the value, B keeps the one set; closed as 0x821BAEA0 does, where only A drops the listeners
void PopupKey(PPCContext& ctx, uint32_t code) {
    bool accept = code == kKeyA || code == kKeyEnter;
    if (!accept && code != kKeyB) return;
    int row = g_popup.row;
    int pick = ReadS16(Read32(g_popup.helper + kHelperValues) + kListSelected);
    g_popup.row = -1;
    kernel::CallGuest(ctx, kPopDraw, {DrawStack(), 0, 0, 0, 0});
    kernel::CallGuest(ctx, kPopInput, {InputStack(), 0, 0});
    if (!accept) return;
    kernel::CallGuest(ctx, kRemoveListeners, {Read32(g_popup.helper + kHelperPopup)});
    if (pick < 0 || pick == g_popup.current || !ListOk()) return;
    rows::Choose(NativeRow(row), pick);
    RefreshValues(ctx);
}

// Delete on a bind row unbinds it
void NativeUnbind(PPCContext& ctx) {
    int row = Selected();
    if (g_native.mode < 0 || !ListOk() || row < 0 || NativeRow(row).kind != rows::Kind::Bind) return;
    rows::ClearBind(NativeRow(row).id);
    RefreshValues(ctx);
    kernel::CallGuest(ctx, kSoundPlay, {Read32(kSoundSystem), kSoundChange});
}

// menu thread, each frame: a bind row's capture ends with a key, Escape, or the page going away
void NativeTick(PPCContext& ctx, uint32_t) {
    profiles::Tick(ctx);
    keyboard::Tick(ctx);
    TickMainMenu(ctx);
    // a change from the panel shows at once
    static uint32_t seen = 0;
    uint32_t revision = SettingsRevision();
    if (revision != seen && g_native.mode >= 0 && ListOk()) RefreshValues(ctx);
    seen = revision;
    bool unbind = keys::TakeDelete();
    if (g_native.capture < 0) {
        if (unbind) NativeUnbind(ctx);
        return;
    }
    int vk = keys::Captured();
    if (!vk) return;
    int row = g_native.capture;
    g_native.capture = -1;
    if (g_native.mode < 0 || !ListOk()) return;
    if (vk > 0) rows::SetBind(NativeRow(row).id, uint8_t(vk));
    RefreshValues(ctx);
    NativeHelp(ctx);
}

const bool g_tick = (g_menu_tick = NativeTick, true);

// swaps the sender for CONTROLLER SETUP's button so the stock press opens the controller page, which Customise then
// takes over
void OpenNative(PPCContext& ctx, uint8_t* base, uint32_t page, int mode, PPCFunc* press) {
    uint32_t control = FindWidget(page, kCrcControlSetup);
    if (!control) {
        if (mode != kAchievementsPage) OpenMenuPage(mode);
        return;
    }
    g_pending = mode;
    ctx.r[4] = control;
    press(ctx, base);
    g_pending = -1;
}

// Watermark: B_Text mirrors "{A} SELECT" at the bottom left of P_Main; the prompt setter empties it on every call,
// so this follows each one

constexpr uint32_t kCrcNavigation = 0xDEB8BA8Bu;  // A_Navigation
constexpr uint32_t kCrcBackText = 0xF3EC853Bu;    // B_Text, empty on P_Main
constexpr uint32_t kVtTextElement = 0x82078CC8u;  // element type 8
constexpr uint32_t kElementAreaShown = 0x30u;
constexpr const char* kWatermark = "XEX Recomp V2";

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
PPC_EXTERN_FUNC(__imp__sub_821303B0);
PPC_EXTERN_FUNC(__imp__sub_821304D8);
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
PPC_EXTERN_FUNC(__imp__sub_821BAEA0);

// base menu init: resolves the pages, attaches listeners and widgets; the pages exist from here on
PPC_FUNC(sub_8216FA60) {
    uint32_t self = uint32_t(ctx.r[3]);
    __imp__sub_8216FA60(ctx, base);
    if (uint32_t(ctx.r[3]) == 1) AddEntriesFor(ctx, self);
}

namespace {

// the draw stack's top page after a push or pop, which every page shown or backed out of goes through
void UpdateTopPage(PPCContext& ctx, uint32_t draw) {
    uint32_t pages = Read32(draw + kDrawInput) + kDrawnPages;
    UpdateBottomFrame(uint32_t(kernel::CallGuest(ctx, kInputTop, {pages, 0, 0})));
}

}  // namespace

// a page pushed on the draw stack, (draw, page, ...)
PPC_FUNC(sub_821303B0) {
    uint32_t draw = uint32_t(ctx.r[3]);
    __imp__sub_821303B0(ctx, base);
    UpdateTopPage(ctx, draw);
}

// the draw stack's top popped, (draw, ...)
PPC_FUNC(sub_821304D8) {
    uint32_t draw = uint32_t(ctx.r[3]);
    __imp__sub_821304D8(ctx, base);
    UpdateTopPage(ctx, draw);
}

// the User page hides its multiplayer slot in this build: it becomes RECOMP SETTINGS
PPC_FUNC(sub_821E31D0) {
    uint32_t self = uint32_t(ctx.r[3]);
    __imp__sub_821E31D0(ctx, base);
    ShowEntry(ctx, self);
}

// captions for our entries, after the page is localised
PPC_FUNC(sub_8216F570) {
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_8216F570(ctx, base);
    uint32_t crc = Read32(page + kPageNameCrc);
    if (crc == kCrcEditProfile || crc == kCrcMainPage || crc == kCrcMultiplayerPage) SetEntryCaptions(ctx, page);
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
    Leave();
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
    // a section goes back to the list of them
    bool section = g_native.mode >= 0 && g_native.section >= 0;
    if (section && uint32_t(ctx.r[3]) == g_native.obj && ListOk()) {
        ShowSection(ctx, -1);
        ctx.r[3] = 0;
        return;
    }
    bool ours = g_native.mode >= 0;
    Leave();
    __imp__sub_821D65F0(ctx, base);
    if (ours) Restore(ctx, g_native.page);
}

// help bar prompt setter, r4 the page
PPC_FUNC(sub_821BBA70) {
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_821BBA70(ctx, base);
    SetWatermark(ctx, page);
    if (Read32(page + kPageNameCrc) == kCrcMainPage) profiles::NoteMainMenu();
}

// the value list's key up (helper, widget, event)
PPC_FUNC(sub_821BAEA0) {
    bool ours = g_popup.row >= 0 && g_native.mode >= 0 && uint32_t(ctx.r[3]) == g_popup.helper;
    if (!ours) {
        __imp__sub_821BAEA0(ctx, base);
        return;
    }
    PopupKey(ctx, Read32(uint32_t(ctx.r[5])));
    ctx.r[3] = 1;
}
