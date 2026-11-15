// ige.xex: the settings entries in the editor's start menu (P_StartMenu of StartMenuInterfaceHandler), under EXIT EDITOR
#include "common/menu.h"
#include "common/widgets.h"
#include "kernel/kernel.h"

using namespace fcip;
using namespace fcip::ui;

namespace {

constexpr uint32_t kCrcStartMenu = 0xA3E9B4F1u;
constexpr uint32_t kCrcControls = 0xB297004Cu;
constexpr uint32_t kCrcHintsTips = 0x174005D2u;  // the template for the clone
constexpr uint32_t kCrcExitEditor = 0xD23F714Cu;
constexpr uint32_t kHandlerPage = 0xFCu;

// showing a page captions every child again, so ours follow each time
void AddStartMenuEntries(PPCContext& ctx, uint32_t handler) {
    uint32_t page = Read32(handler + kHandlerPage);
    if (!page || Read32(page + kPageNameCrc) != kCrcStartMenu) return;
    AddEntries(ctx, page, FindWidget(page, kCrcExitEditor), kCrcHintsTips, kCrcControls,
               {kCrcRecompSettings, kCrcMouseKeyboard, kCrcControllerSettings});
    SetEntryCaptions(ctx, page);
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_820C08C8);
PPC_EXTERN_FUNC(__imp__sub_820C2C88);

// shows a page for an editor interface handler (r3), which keeps it at +0xFC
PPC_FUNC(sub_820C08C8) {
    uint32_t handler = uint32_t(ctx.r[3]);
    __imp__sub_820C08C8(ctx, base);
    AddStartMenuEntries(ctx, handler);
}

// start menu press, r4 the widget: an unknown name only plays the click and returns 1, so ours open the panel after
PPC_FUNC(sub_820C2C88) {
    int page = PageFor(Read32(uint32_t(ctx.r[4]) + kWidgetNameCrc));
    __imp__sub_820C2C88(ctx, base);
    if (page >= 0) OpenMenuPage(page);
}
