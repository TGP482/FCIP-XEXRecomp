// default.xex: EXIT on the main menu asks through the game's own message box, Yes closes the window. MAP EDITOR asks
// the same way before the editor launches. CUSTOM CAMPAIGN
// presses STORY MODE, or EXPANSION CAMPAIGN for an Evolution level, so the game's own new game and save slot pages
// run, and the level they start becomes the first custom level. Its page reads CUSTOM CAMPAIGN and LOAD A MAP lists
// the custom levels. MULTIPLAYER's ONLINE reads ONLINE JOIN IP and types the system link join address on the virtual
// keyboard.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

#include "app/app.h"
#include "common/custom_levels.h"
#include "common/menu.h"
#include "common/widgets.h"
#include "core/log.h"
#include "game/custom_campaign.h"
#include "game/exit_menu.h"
#include "game/virtual_keyboard.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using namespace fcip::ui;

namespace {

// the message boxes, as CryPreGame's own (0x821B4518) reach them
constexpr uint32_t kSystemUi = 0x340u;
constexpr uint32_t kUiMessageBoxes = 0x0Cu;
constexpr uint32_t kYesNo = 4;
constexpr uint32_t kAnyPad = 0xFFFFFFFFu;
constexpr uint32_t kExitId = 0x45584954u;
constexpr uint32_t kEditorId = 0x45444954u;
// CryPreGame's message box listener: slot 0 on A, slot 1 on B (a no op)
constexpr uint32_t kPreGameListener = 0xECu;
constexpr uint32_t kPages = 0x50u;

constexpr const char* kExitQuestion = "Are you sure you want to exit to the desktop?";
constexpr const char* kEditorQuestion = "Are you sure you want to launch the map editor?";

// B_MapEditor, whose press (821B1FD0) runs PreGame_LaunchEditor
constexpr uint32_t kCrcMapEditor = 0x08D70D76u;

// CryPreGame's menu system, as its help setter (821B7B40) reaches the page on top
constexpr uint32_t kMenuSystem = 0x10u;
constexpr uint32_t kMenuSystemUi = 0x18u;
constexpr uint32_t kUiActivePages = 0x38u;

constexpr uint32_t kJoinIpChars = 63;  // the F10 field's
constexpr const char* kJoinIpTitle = "ENTER THE HOST'S IP ADDRESS";
constexpr const char* kOnlineHelp = "Enter the host's IP address, then join their game from System Link. Current IP: ";

// the story buttons on P_Main
constexpr uint32_t kCrcStoryMode = 0x17130B2Fu;
constexpr uint32_t kCrcExpansion = 0x28A23D05u;

// CryPreGame's P_StoryMode is pages[0]: its title, LOAD A MAP, its list and the level each row starts
constexpr uint32_t kCrcStoryTitle = 0x06326AC4u;  // B_TitleButton
constexpr uint32_t kLoadMapButton = 0xECu;
constexpr uint32_t kMapList = 0xF4u;
constexpr uint32_t kMapLevels = 0xF8u;  // std::vector<std::string>, begin +4, end +8
constexpr uint32_t kPageListener = 0x04u;  // the page shown listener inside CryPreGame
constexpr uint32_t kStringBytes = 0x1Cu;
constexpr uint32_t kNameBytes = 0x100u;

// an empty std::wstring: inline buffer at +0x04, length +0x14, capacity +0x18
constexpr uint32_t kWstringBytes = 0x1Cu;
constexpr uint32_t kInlineCapacity = 7;

uint32_t EmptyText() {
    static const uint32_t text = [] {
        uint32_t block = mem::AllocSystem(kWstringBytes);
        if (!block) return 0u;
        for (uint32_t off = 0; off < kWstringBytes; off += 4) Write32(block + off, 0);
        Write32(block + 0x18, kInlineCapacity);
        return block;
    }();
    return text;
}

void Ask(PPCContext& ctx, uint32_t self, uint32_t id, const char* text) {
    uint32_t boxes = Read32(Read32(Read32(SYM_Game_pointer) + kSystemUi) + kUiMessageBoxes);
    uint32_t empty = EmptyText();
    uint32_t question = View(text);
    if (!boxes || !empty || !question) {
        LOGE("main menu: no message box, %08X", boxes);
        return;
    }
    uint32_t listener = self + kPreGameListener;
    kernel::CallGuest(ctx, SYM_MessageBox_Show, {boxes, id, empty, question, kYesNo, 0, listener, empty, empty, 0, 0, 0,
                                                 kAnyPad});
}

// the story button for the first custom level's campaign, 0 with no custom levels
uint32_t CustomCampaignButton(uint32_t self) {
    std::string campaign = fcip::BeginCustomCampaign();
    if (campaign.empty()) return 0;
    uint32_t page = Read32(Read32(self + kPages) + 4);
    return FindWidget(page, fcip::EvolutionLevel(campaign) ? kCrcExpansion : kCrcStoryMode);
}

uint32_t StoryPage(uint32_t self) { return Read32(Read32(self + kPages)); }

void SetStoryTitle(PPCContext& ctx, uint32_t self) {
    uint32_t title = FindWidget(StoryPage(self), kCrcStoryTitle);
    SetCaption(ctx, Read32(title + kWidgetElement), CaptionFor(kCrcCustomCampaign));
}

std::string Upper(std::string text) {
    for (char& c : text) c = char(std::toupper(uint8_t(c)));
    return text;
}

// LOAD A MAP's rows and the levels they start, as the game fills them (821B7FD8) from the unlocked levels
void ListCustomLevels(PPCContext& ctx, uint32_t self) {
    static const uint32_t scratch = mem::AllocSystem(kStringBytes + 4 + kNameBytes);
    if (!scratch) return;
    uint32_t str = scratch;
    uint32_t iterator = scratch + kStringBytes;
    uint32_t name = iterator + 4;
    uint32_t levels = self + kMapLevels;
    uint32_t list = Read32(self + kMapList);
    kernel::CallGuest(ctx, SYM_LevelList_Erase, {iterator, levels, Read32(levels + 4), Read32(levels + 8)});
    kernel::CallGuest(ctx, SYM_UIList_Clear, {list});
    for (const fcip::CustomLevel& level : fcip::FindCustomLevels()) {
        size_t n = std::min<size_t>(level.name.size(), kNameBytes - 1);
        for (size_t i = 0; i < n; i++) Write8(name + uint32_t(i), uint8_t(level.name[i]));
        Write8(name + uint32_t(n), 0);
        kernel::CallGuest(ctx, SYM_String_Init, {str, name});
        kernel::CallGuest(ctx, SYM_LevelList_PushBack, {levels, str});
        kernel::CallGuest(ctx, SYM_String_Free, {str, 1, 0});
        kernel::CallGuest(ctx, SYM_UIList_AddRow, {list, Text(Upper(level.name).c_str()), 0});
    }
}

// UTF-16 for the keyboard: slot 0 the starting text, slot 1 the line above it
uint32_t KeyboardText(uint32_t slot, const std::string& text) {
    constexpr uint32_t kSlotBytes = (kJoinIpChars + 1) * 2;
    static const uint32_t block = mem::AllocSystem(kSlotBytes * 2);
    uint32_t addr = block + slot * kSlotBytes;
    uint32_t n = std::min<uint32_t>(uint32_t(text.size()), kJoinIpChars);
    for (uint32_t i = 0; i < n; i++) kernel::W16(addr + i * 2, uint8_t(text[i]));
    kernel::W16(addr + n * 2, 0);
    return addr;
}

// ONLINE JOIN IP's description, while it is focused
struct OnlineHelp {
    uint32_t self = 0;  // CryPreGame
    uint32_t vtable = 0;  // its own, gone once the menus are
    uint32_t widget = 0;
    uint32_t revision = 0;  // the settings revision the description shows
    bool pending = false;  // the join IP changed since
};
OnlineHelp g_online;

std::string JoinIp() {
    std::lock_guard ui(app::UiLock());
    return app::Get().join_ip;
}

void ShowOnlineHelp(PPCContext& ctx, uint32_t page) {
    std::string ip = JoinIp();
    std::string text = kOnlineHelp + (ip.empty() ? std::string("NONE") : ip);
    uint32_t help = View(text.c_str());
    if (help) kernel::CallGuest(ctx, SYM_UIPage_SetHelp, {g_online.self, page, help, 0});
    g_online.pending = false;
}

bool OnJoinIp(PPCContext&, uint32_t text, uint32_t length) {
    std::string ip;
    for (uint32_t i = 0; i < length; i++) ip += char(kernel::R16(text + i * 2));
    std::lock_guard ui(app::UiLock());
    app::Get().join_ip = ip;
    app::Save();
    g_online.pending = true;
    return true;
}

void AskJoinIp(PPCContext& ctx) {
    fcip::keyboard::Open(ctx, KeyboardText(0, JoinIp()), KeyboardText(1, kJoinIpTitle), kJoinIpChars, OnJoinIp, true);
}

uint32_t TopPage(PPCContext& ctx, uint32_t self) {
    uint32_t ui = Read32(Read32(self + kMenuSystem) + kMenuSystemUi);
    return uint32_t(kernel::CallGuest(ctx, SYM_UIInputStack_Top, {ui + kUiActivePages, 0, 0}));
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_821B1FD0);
PPC_EXTERN_FUNC(__imp__sub_821B7B40);
PPC_EXTERN_FUNC(__imp__sub_821B3380);
PPC_EXTERN_FUNC(__imp__sub_821B7CC0);
PPC_EXTERN_FUNC(__imp__sub_821B7FD8);

// main menu press, r4 the widget
PPC_FUNC(sub_821B1FD0) {
    uint32_t crc = Read32(uint32_t(ctx.r[4]) + kWidgetNameCrc);
    if (crc == kCrcExit) {
        Ask(ctx, uint32_t(ctx.r[3]), kExitId, kExitQuestion);
        return;
    }
    if (crc == kCrcMapEditor) {
        Ask(ctx, uint32_t(ctx.r[3]), kEditorId, kEditorQuestion);
        return;
    }
    if (crc == kCrcOnline) {
        AskJoinIp(ctx);
        return;
    }
    if (crc != kCrcCustomCampaign) {
        __imp__sub_821B1FD0(ctx, base);
        return;
    }
    uint32_t self = uint32_t(ctx.r[3]);
    uint32_t button = CustomCampaignButton(self);
    if (!button) return;
    ctx.r[4] = button;
    __imp__sub_821B1FD0(ctx, base);
    uint64_t result = ctx.r[3];
    SetStoryTitle(ctx, self);
    ctx.r[3] = result;
}

// main, multiplayer and story menu help text, r4 the focused widget; back on the main menu a CUSTOM CAMPAIGN start
// was left
PPC_FUNC(sub_821B7B40) {
    uint32_t self = uint32_t(ctx.r[3]);
    uint32_t sender = uint32_t(ctx.r[4]);
    __imp__sub_821B7B40(ctx, base);
    uint32_t crc = Read32(sender + kWidgetNameCrc);
    uint32_t page = Read32(Read32(self + kPages) + 4);
    if (FindWidget(page, crc) == sender) fcip::CancelCustomCampaign();
    g_online = {};
    if (crc == kCrcOnline) {
        g_online = {self, Read32(self), sender, fcip::SettingsRevision(), false};
        ShowOnlineHelp(ctx, TopPage(ctx, self));
        return;
    }
    if (crc != kCrcExit && crc != kCrcCustomCampaign) return;
    if (uint32_t help = View(HelpFor(crc))) kernel::CallGuest(ctx, SYM_UIPage_SetHelp, {self, page, help, 0});
}

// a CryPreGame page shown, r3 its page listener, r4 the page; P_StoryMode turns LOAD A MAP off with no level unlocked
PPC_FUNC(sub_821B7CC0) {
    uint32_t self = uint32_t(ctx.r[3]) - kPageListener;
    uint32_t page = uint32_t(ctx.r[4]);
    __imp__sub_821B7CC0(ctx, base);
    if (!fcip::CustomCampaignPending() || page != StoryPage(self)) return;
    uint64_t result = ctx.r[3];
    kernel::CallGuest(ctx, SYM_UIWidget_Enable, {Read32(self + kLoadMapButton), 1});
    ctx.r[3] = result;
}

// LOAD A MAP's list filled, r3 CryPreGame
PPC_FUNC(sub_821B7FD8) {
    if (fcip::CustomCampaignPending()) {
        ListCustomLevels(ctx, uint32_t(ctx.r[3]));
        return;
    }
    __imp__sub_821B7FD8(ctx, base);
}

// message box A, r3 CryPreGame's listener, r4 the box's id
PPC_FUNC(sub_821B3380) {
    uint32_t id = uint32_t(ctx.r[4]);
    if (id == kEditorId) {
        kernel::CallGuest(ctx, SYM_PreGame_LaunchEditor, {uint32_t(ctx.r[3]) - kPreGameListener});
        return;
    }
    if (id != kExitId) {
        __imp__sub_821B3380(ctx, base);
        return;
    }
    PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
}

// the description waits for the keyboard page to close; another menu's object in CryPreGame's place drops it
void fcip::TickMainMenu(PPCContext& ctx) {
    if (!g_online.self) return;
    uint32_t revision = SettingsRevision();
    if (revision != g_online.revision) g_online.pending = true;
    g_online.revision = revision;
    if (!g_online.pending) return;
    if (Read32(g_online.self) != g_online.vtable) {
        g_online = {};
        return;
    }
    uint32_t top = TopPage(ctx, g_online.self);
    if (FindWidget(top, kCrcOnline) == g_online.widget) ShowOnlineHelp(ctx, top);
}
