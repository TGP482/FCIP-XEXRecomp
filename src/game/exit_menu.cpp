// default.xex: EXIT on the main menu asks through the game's own message box, Yes closes the window
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/widgets.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using namespace fcip::ui;

namespace {

// the message boxes, as CryPreGame's own (0x821B4518) reach them
constexpr uint32_t kSystem = 0x8291234Cu;
constexpr uint32_t kSystemUi = 0x340u;
constexpr uint32_t kUiMessageBoxes = 0x0Cu;
// (boxes, id, title, text, type, priority, listener, A text, B text, 0, 0, 0, pad), the strings std::wstring
constexpr uint32_t kShowMessageBox = 0x8216B168u;
constexpr uint32_t kYesNo = 4;
constexpr uint32_t kAnyPad = 0xFFFFFFFFu;
constexpr uint32_t kExitId = 0x45584954u;
// CryPreGame's message box listener: slot 0 on A, slot 1 on B (a no op)
constexpr uint32_t kPreGameListener = 0xECu;
constexpr uint32_t kPages = 0x50u;
// (menu, page, std::wstring*, 0): the page's A_Help text
constexpr uint32_t kSetPageHelp = 0x8216E3D8u;

constexpr const char* kQuestion = "Are you sure you want to exit to the desktop?";

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

void AskExit(PPCContext& ctx, uint32_t self) {
    uint32_t boxes = Read32(Read32(Read32(kSystem) + kSystemUi) + kUiMessageBoxes);
    uint32_t empty = EmptyText();
    uint32_t question = View(kQuestion);
    if (!boxes || !empty || !question) {
        LOGE("exit: no message box, %08X", boxes);
        return;
    }
    kernel::CallGuest(ctx, kShowMessageBox, {boxes, kExitId, empty, question, kYesNo, 0, self + kPreGameListener, empty,
                                             empty, 0, 0, 0, kAnyPad});
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_821B1FD0);
PPC_EXTERN_FUNC(__imp__sub_821B7B40);
PPC_EXTERN_FUNC(__imp__sub_821B3380);

// main menu press, r4 the widget
PPC_FUNC(sub_821B1FD0) {
    if (Read32(uint32_t(ctx.r[4]) + kWidgetNameCrc) != kCrcExit) {
        __imp__sub_821B1FD0(ctx, base);
        return;
    }
    AskExit(ctx, uint32_t(ctx.r[3]));
}

// main menu help text, r4 the focused widget
PPC_FUNC(sub_821B7B40) {
    uint32_t self = uint32_t(ctx.r[3]);
    uint32_t sender = uint32_t(ctx.r[4]);
    __imp__sub_821B7B40(ctx, base);
    if (Read32(sender + kWidgetNameCrc) != kCrcExit) return;
    uint32_t page = Read32(Read32(self + kPages) + 4);
    if (uint32_t help = View(HelpFor(kCrcExit))) kernel::CallGuest(ctx, kSetPageHelp, {self, page, help, 0});
}

// message box A, r4 the box's id
PPC_FUNC(sub_821B3380) {
    if (uint32_t(ctx.r[4]) != kExitId) {
        __imp__sub_821B3380(ctx, base);
        return;
    }
    LOGR("exit: closing the window");
    PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
}
