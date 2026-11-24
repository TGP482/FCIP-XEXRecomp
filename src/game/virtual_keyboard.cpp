// default.xex: the original Xbox virtual keyboard page (P_VirtualKeyBoard), still built and updated by the 360 menus
// but never opened, takes the title's XShowKeyboardUI requests (the CHEATS code entry, also in the pause menu) in place
// of the system keyboard, and the profile names the profile list asks for
#include "game/virtual_keyboard.h"

#include <algorithm>
#include <string>

#include "common/keys.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the virtual keyboard menu (constructor 82170098)
constexpr uint32_t kKeyboardListener = 0x60;  // left unset by the constructor, read by back and accept
constexpr uint32_t kKeyboardInfo = 0xB4;  // L_InfoText
constexpr uint32_t kKeyboardLine = 0xB8;  // L_Text
constexpr uint32_t kKeyboardText = 0xD8;  // std::wstring
constexpr uint32_t kKeyboardCanBack = 0xF4;  // B backs out and its prompt shows (82171828)
// both set: the first letter typed replaces the starting text, as the original's default name
constexpr uint32_t kKeyboardKeepStart = 0x100;
constexpr uint32_t kKeyboardStartShown = 0x104;
constexpr uint32_t kKeyboardMaxLength = 0x10C;
// the letter set the ring shows, of the count loaded: uppercase, lowercase, symbols
constexpr uint32_t kKeyboardLetterSet = 0x110;
constexpr uint32_t kKeyboardLetterSets = 0x114;
constexpr uint32_t kUppercase = 0;
constexpr uint32_t kLowercase = 1;
constexpr uint32_t kSymbols = 2;
constexpr uint32_t kMenuShowPage = 0x2C;  // vtable: (menu, page, 1, 1)
constexpr uint32_t kMenuClose = 0x28;     // vtable: (menu, 0, 0)
constexpr uint32_t kMenuDelete = 0x18;    // vtable: (menu, 1)
constexpr uint32_t kMenuLoad = 0x1C;      // vtable: (menu)
constexpr uint32_t kMenuUpdate = 0x34;    // vtable: (menu, frame)
constexpr uint32_t kMenuGame = 0x0C;
constexpr uint32_t kMenuSystem = 0x10;
constexpr uint32_t kKeyboardBytes = 0x12C;

// std::wstring
constexpr uint32_t kStringData = 0x04;
constexpr uint32_t kStringLength = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kStringInline = 8;  // a capacity below this keeps the text in place of the pointer

// XOVERLAPPED
constexpr uint32_t kOverlappedResult = 0x00;
constexpr uint32_t kOverlappedError = 0x18;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorIoPending = 0x3E5;
constexpr uint32_t kErrorCancelled = 0x4C7;

constexpr uint32_t kMaxUnits = 256;

// the real keyboard's control codes as the layout types them
constexpr char16_t kBackspace = 0x08;
constexpr char16_t kTab = 0x09;
constexpr char16_t kEnter = 0x0D;
constexpr char16_t kEscape = 0x1B;

// the open entry, menu thread only: an XShowKeyboardUI request, or ours with its accept
struct Request {
    bool open = false;
    uint32_t result = 0;  // UTF-16 the title reads, units long with the terminator
    uint32_t units = 0;
    uint32_t overlapped = 0;
    fcip::keyboard::Accept accept = nullptr;
};
Request g_request;
uint32_t g_keyboard = 0;
uint32_t g_in_game = 0;  // the keyboard made for CryInGame
int g_upper = -1;  // Caps Lock against Shift as the ring last followed it, -1 for not yet

uint32_t StringData(uint32_t str) {
    return R32(str + kStringCapacity) < kStringInline ? str + kStringData : R32(str + kStringData);
}

uint32_t WideLength(uint32_t text) {
    uint32_t n = 0;
    while (text && n < kMaxUnits && R16(text + n * 2)) n++;
    return n;
}

void Finish(uint32_t error, uint32_t text, uint32_t length) {
    uint32_t n = std::min(length, g_request.units - 1);
    for (uint32_t i = 0; i < n; i++) W16(g_request.result + i * 2, R16(text + i * 2));
    W16(g_request.result + n * 2, 0);
    W32(g_request.overlapped + kOverlappedError, error);
    W32(g_request.overlapped + kOverlappedResult, error);
    g_request = {};
}

// the entry ends unanswered
void Cancel() {
    if (g_request.overlapped) Finish(kErrorCancelled, 0, 0);
    g_request = {};
}

bool AllSpaces(uint32_t text, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        if (R16(text + i * 2) != ' ') return false;
    }
    return true;
}

// the typed text to the entry's owner: true when the page should close
bool HandOver(PPCContext& ctx, uint32_t keyboard) {
    uint32_t text = keyboard + kKeyboardText;
    uint32_t data = StringData(text);
    uint32_t length = R32(text + kStringLength);
    if (g_request.overlapped) {
        Finish(kErrorSuccess, data, length);
        return true;
    }
    if (!g_request.accept || !g_request.accept(ctx, data, length)) return false;
    g_request = {};
    return true;
}

void ClosePage(PPCContext& ctx) { CallGuest(ctx, R32(R32(g_keyboard) + kMenuClose), {g_keyboard, 0, 0}); }

// a std::wstring holding one character in place
uint32_t OneCharacter(char16_t c) {
    static const uint32_t str = mem::AllocSystem(0x20);
    W16(str + kStringData, c);
    W16(str + kStringData + 2, 0);
    W32(str + kStringLength, 1);
    W32(str + kStringCapacity, kStringInline - 1);
    return str;
}

void ShowLetterSet(PPCContext& ctx, uint32_t set) {
    if (set >= R32(g_keyboard + kKeyboardLetterSets)) return;
    W32(g_keyboard + kKeyboardLetterSet, set);
    CallGuest(ctx, SYM_VirtualKeyboard_ShowLetters, {g_keyboard});
}

uint32_t TypedCase() { return fcip::keys::TypingUpper() ? kUppercase : kLowercase; }

// the ring follows Caps Lock and Shift when they change, unless it shows the symbols
void FollowCase(PPCContext& ctx) {
    int upper = fcip::keys::TypingUpper() ? 1 : 0;
    if (upper == g_upper) return;
    g_upper = upper;
    uint32_t set = R32(g_keyboard + kKeyboardLetterSet);
    if (set != kSymbols && set != TypedCase()) ShowLetterSet(ctx, TypedCase());
}

// false once the page closed
bool Type(PPCContext& ctx, char16_t c) {
    if (c == kBackspace) {
        CallGuest(ctx, SYM_VirtualKeyboard_Delete, {g_keyboard});
    } else if (c == kTab) {
        bool symbols = R32(g_keyboard + kKeyboardLetterSet) == kSymbols;
        ShowLetterSet(ctx, symbols ? TypedCase() : kSymbols);
    } else if (c == kEscape) {
        if (!R32(g_keyboard + kKeyboardCanBack)) return true;
        ClosePage(ctx);
        return false;
    } else if (c == kEnter) {
        uint32_t text = g_keyboard + kKeyboardText;
        if (AllSpaces(StringData(text), R32(text + kStringLength)) || !HandOver(ctx, g_keyboard)) return true;
        ClosePage(ctx);
        return false;
    } else if (c >= ' ' && c < 0x7F) {
        CallGuest(ctx, SYM_VirtualKeyboard_AddText, {g_keyboard, OneCharacter(c)});
    }
    return true;
}

bool OpenPage(PPCContext& ctx, uint32_t start, uint32_t info, uint32_t max_length, bool can_back) {
    uint32_t keyboard = R32(SYM_VirtualKeyboard_pointer);
    if (!keyboard || g_request.open) return false;
    g_keyboard = keyboard;
    g_upper = -1;
    W32(keyboard + kKeyboardListener, 0);
    W32(keyboard + kKeyboardCanBack, can_back ? 1 : 0);
    CallGuest(ctx, R32(R32(keyboard) + kMenuShowPage), {keyboard, 0, 1, 1});
    uint32_t length = WideLength(start);
    uint32_t text = start ? start : StringData(keyboard + kKeyboardText);
    CallGuest(ctx, SYM_WString_AssignText, {keyboard + kKeyboardText, text, length});
    W32(keyboard + kKeyboardKeepStart, length ? 1 : 0);
    W32(keyboard + kKeyboardStartShown, length ? 1 : 0);
    W32(keyboard + kKeyboardMaxLength, max_length);
    CallGuest(ctx, SYM_UIText_SetText, {R32(keyboard + kKeyboardLine), StringData(keyboard + kKeyboardText)});
    fcip::keyboard::SetInfo(ctx, info);
    return true;
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(XShowKeyboardUI));
PPC_EXTERN_FUNC(GUEST_ORIG(VirtualKeyboard_Close));
PPC_EXTERN_FUNC(GUEST_ORIG(InGame_Load));
PPC_EXTERN_FUNC(GUEST_ORIG(InGame_Update));
PPC_EXTERN_FUNC(GUEST_ORIG(InGame_Destroy));
PPC_EXTERN_FUNC(GUEST_ORIG(MenuScan_ReadPad));

bool fcip::keyboard::Open(PPCContext& ctx, uint32_t start, uint32_t info, uint32_t max_length, Accept accept,
                          bool can_back) {
    if (!OpenPage(ctx, start, info, max_length, can_back)) return false;
    g_request = {true, 0, 0, 0, accept};
    return true;
}

void fcip::keyboard::Tick(PPCContext& ctx) {
    // a level starting frees the menus' page (821B16A8) without its close
    if (g_request.open && R32(SYM_VirtualKeyboard_pointer) != g_keyboard) Cancel();
    keys::SetTextEntry(g_request.open);
    if (!g_request.open) return;
    FollowCase(ctx);
    for (char16_t c : keys::TakeText()) {
        if (!Type(ctx, c)) break;
    }
}

void fcip::keyboard::SetInfo(PPCContext& ctx, uint32_t info) {
    uint32_t line = g_keyboard ? R32(g_keyboard + kKeyboardInfo) : 0;
    if (info && line) CallGuest(ctx, SYM_UIText_SetText, {line, info});
}

// (user, flags, starting text, title, description, result, result units, overlapped)
PPC_FUNC(GUEST(XShowKeyboardUI)) {
    uint32_t units = uint32_t(ctx.r[9]);
    uint32_t result = uint32_t(ctx.r[8]);
    uint32_t start = uint32_t(ctx.r[5]);
    uint32_t info = ctx.r[7] ? uint32_t(ctx.r[7]) : uint32_t(ctx.r[6]);
    if (!units || !OpenPage(ctx, start, info, units - 1, true)) {
        GUEST_ORIG(XShowKeyboardUI)(ctx, base);
        return;
    }
    g_request = {true, result, units, uint32_t(ctx.r[10]), nullptr};
    W32(g_request.overlapped + kOverlappedResult, kErrorIoPending);
    ctx.r[3] = kErrorIoPending;
}

// the 360 menu scan reads a held stick as a direction and then ignores the buttons, so A could never type the letter
// the stick points at
PPC_FUNC(GUEST(MenuScan_ReadPad)) {
    if (g_request.open) ctx.r[9] = 0;
    GUEST_ORIG(MenuScan_ReadPad)(ctx, base);
}

// the page's back out, and its close after an accept
PPC_FUNC(GUEST(VirtualKeyboard_Close)) {
    Cancel();
    GUEST_ORIG(VirtualKeyboard_Close)(ctx, base);
}

// r31 the keyboard, before its accept hands the text to a listener (none is ever set on the 360): true closes the
// page, false keeps it open
bool VirtualKeyboardAccept(PPCContext& ctx, uint8_t*) { return HandOver(ctx, uint32_t(ctx.r[31])); }

// (menu): the single player in game menus never make the keyboard, so their CHEATS entry had none; made into their
// menu system as the split screen menus do (821AB4A8)
PPC_FUNC(GUEST(InGame_Load)) {
    uint32_t menu = uint32_t(ctx.r[3]);
    GUEST_ORIG(InGame_Load)(ctx, base);
    if (!ctx.r[3] || R32(SYM_VirtualKeyboard_pointer)) return;
    uint32_t memory = uint32_t(CallGuest(ctx, SYM_Guest_Alloc, {kKeyboardBytes}));
    if (!memory) return;
    uint32_t game = R32(menu + kMenuGame);
    uint32_t system = R32(menu + kMenuSystem);
    g_in_game = uint32_t(CallGuest(ctx, SYM_VirtualKeyboard_New, {memory, 0, game, system}));
    W32(SYM_VirtualKeyboard_pointer, g_in_game);
    CallGuest(ctx, R32(R32(g_in_game) + kMenuLoad), {g_in_game});
}

// (menu, frame)
PPC_FUNC(GUEST(InGame_Update)) {
    uint32_t frame = uint32_t(ctx.r[4]);
    GUEST_ORIG(InGame_Update)(ctx, base);
    if (g_in_game) CallGuest(ctx, R32(R32(g_in_game) + kMenuUpdate), {g_in_game, frame});
}

// (menu): its keyboard goes with it, the front end makes its own again
PPC_FUNC(GUEST(InGame_Destroy)) {
    if (g_in_game && R32(SYM_VirtualKeyboard_pointer) == g_in_game) {
        W32(SYM_VirtualKeyboard_pointer, 0);
        CallGuest(ctx, R32(R32(g_in_game) + kMenuDelete), {g_in_game, 1});
    }
    g_in_game = 0;
    GUEST_ORIG(InGame_Destroy)(ctx, base);
}
