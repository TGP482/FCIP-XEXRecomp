// default.xex: the original Xbox virtual keyboard page (P_VirtualKeyBoard), still built and updated by the 360 menus
// but never opened, takes the title's XShowKeyboardUI requests (the CHEATS code entry) in place of the system keyboard
#include <algorithm>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the virtual keyboard menu (constructor 82170098)
constexpr uint32_t kKeyboardInfo = 0xB4;  // L_InfoText
constexpr uint32_t kKeyboardLine = 0xB8;  // L_Text
constexpr uint32_t kKeyboardText = 0xD8;  // std::wstring
// both set: the first letter typed replaces the starting text, as the original's default name
constexpr uint32_t kKeyboardKeepStart = 0x100;
constexpr uint32_t kKeyboardStartShown = 0x104;
constexpr uint32_t kKeyboardMaxLength = 0x10C;
constexpr uint32_t kMenuShowPage = 0x2C;  // vtable: (menu, page, 1, 1)

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

// the open XShowKeyboardUI request, menu thread only
struct Request {
    uint32_t result = 0;  // UTF-16 the title reads, units long with the terminator
    uint32_t units = 0;
    uint32_t overlapped = 0;
};
Request g_request;

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
    LOGR("virtual keyboard: %s, %u characters", error == kErrorSuccess ? "accepted" : "cancelled", n);
    g_request = {};
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(XShowKeyboardUI));
PPC_EXTERN_FUNC(GUEST_ORIG(VirtualKeyboard_Close));

// (user, flags, starting text, title, description, result, result units, overlapped)
PPC_FUNC(GUEST(XShowKeyboardUI)) {
    uint32_t keyboard = R32(SYM_VirtualKeyboard_pointer);
    uint32_t units = uint32_t(ctx.r[9]);
    if (!keyboard || !units || g_request.overlapped) {
        GUEST_ORIG(XShowKeyboardUI)(ctx, base);
        return;
    }
    uint32_t start = uint32_t(ctx.r[5]);
    uint32_t info = ctx.r[7] ? uint32_t(ctx.r[7]) : uint32_t(ctx.r[6]);
    g_request = {uint32_t(ctx.r[8]), units, uint32_t(ctx.r[10])};
    W32(g_request.overlapped + kOverlappedResult, kErrorIoPending);

    CallGuest(ctx, R32(R32(keyboard) + kMenuShowPage), {keyboard, 0, 1, 1});
    uint32_t length = WideLength(start);
    CallGuest(ctx, SYM_WString_AssignText, {keyboard + kKeyboardText, start ? start : g_request.result, length});
    W32(keyboard + kKeyboardKeepStart, length ? 1 : 0);
    W32(keyboard + kKeyboardStartShown, length ? 1 : 0);
    W32(keyboard + kKeyboardMaxLength, units - 1);
    CallGuest(ctx, SYM_UIText_SetText, {R32(keyboard + kKeyboardLine), StringData(keyboard + kKeyboardText)});
    uint32_t info_line = R32(keyboard + kKeyboardInfo);
    if (info && info_line) CallGuest(ctx, SYM_UIText_SetText, {info_line, info});
    LOGR("virtual keyboard: opened, up to %u characters", units - 1);
    ctx.r[3] = kErrorIoPending;
}

// the page's back out, and its close after an accept
PPC_FUNC(GUEST(VirtualKeyboard_Close)) {
    if (g_request.overlapped) Finish(kErrorCancelled, 0, 0);
    GUEST_ORIG(VirtualKeyboard_Close)(ctx, base);
}

// r31 the keyboard, before its accept hands the text to a listener (none is ever set on the 360): true closes the page
bool VirtualKeyboardAccept(PPCContext& ctx, uint8_t*) {
    if (!g_request.overlapped) return false;
    uint32_t text = uint32_t(ctx.r[31]) + kKeyboardText;
    Finish(kErrorSuccess, StringData(text), R32(text + kStringLength));
    return true;
}
