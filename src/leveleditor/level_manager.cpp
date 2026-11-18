// ige.xex, the solo level editor: the map manager is the LEVEL MANAGER. OPEN A LEVEL lists the campaign's levels
// and the custom levels in levels\saved in a window over the page, CREATE A NEW LEVEL takes publishing's row and makes
// one from a template, and creating without saving goes, EXIT EDITOR taking its row.
#include <atomic>
#include <cstdint>

#include "common/widgets.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::ui;

namespace {

// P_MapManager's widgets, crc32 of their names
constexpr uint32_t kCrcTitle = 0x06326AC4u;       // B_TitleButton
constexpr uint32_t kCrcCreateNewMap = 0xF5AEBE5Bu;
constexpr uint32_t kCrcEditSavedMap = 0x0DC2C06Eu;
constexpr uint32_t kCrcShareMap = 0xAF3D9AC3u;
constexpr uint32_t kCrcDeleteMap = 0xF6EDFCDCu;
constexpr uint32_t kCrcImportMap = 0xB24710B8u;

struct Row {
    uint32_t crc;
    const char* caption;
};
constexpr Row kRows[] = {
    {kCrcTitle, "LEVEL MANAGER"},
    {kCrcCreateNewMap, "OPEN A LEVEL"},
    {kCrcEditSavedMap, "EDIT A SAVED LEVEL"},
    {kCrcShareMap, "CREATE A NEW LEVEL"},
    {kCrcDeleteMap, "DELETE A LEVEL"},
};

// the map manager's press listener: +0xF8 the list shown, 0 the main one
constexpr uint32_t kShownList = 0xF8;
constexpr uint32_t kMainList = 0;
constexpr uint32_t kPressSound = 0x02E60003;
constexpr uint32_t kScratchBytes = 8;  // a stand in widget: its name crc at +4

std::atomic<bool> g_list_up{false};
std::atomic<bool> g_apply{false};  // the page is shown again
uint32_t g_scratch = 0;

void ApplyRows(PPCContext& ctx, uint32_t page) {
    for (const Row& r : kRows) {
        if (uint32_t widget = FindWidget(page, r.crc)) SetCaption(ctx, Read32(widget + kWidgetElement), r.caption);
    }
    uint32_t import = FindWidget(page, kCrcImportMap);
    MoveWidget(FindWidget(page, kCrcExit), import);
    HideWidget(import);
}

}  // namespace

namespace fcip::editor {

bool LevelListUp() { return g_list_up; }
void CloseLevelList() { g_list_up = false; }
void LevelManagerShown() { g_apply = true; }

void UpdateLevelManager(PPCContext& ctx, uint32_t page) {
    if (!g_apply.exchange(false)) return;
    ApplyRows(ctx, page);
}

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_820CBA80);

// a map manager button pressed: r3 its listener, r4 the widget
PPC_FUNC(sub_820CBA80) {
    uint32_t listener = uint32_t(ctx.r[3]);
    if (!fcip::editor::g_solo || R32(listener + kShownList) != kMainList) return __imp__sub_820CBA80(ctx, base);
    uint32_t crc = R32(uint32_t(ctx.r[4]) + kWidgetNameCrc);
    if (crc == kCrcCreateNewMap) {
        CallGuest(ctx, SYM_Sound_Play, {uint32_t(CallGuest(ctx, SYM_Sound_Get, {})), kPressSound});
        g_list_up = true;
        ctx.r[3] = 0;
        return;
    }
    if (crc == kCrcShareMap) {
        if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
        if (g_scratch) {
            W32(g_scratch + kWidgetNameCrc, kCrcCreateNewMap);
            ctx.r[4] = g_scratch;
        }
    }
    __imp__sub_820CBA80(ctx, base);
}
