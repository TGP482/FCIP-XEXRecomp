// ige.xex: the Add Object tool's budgets follow the level's sectors. Each category caps its objects at a constant sized
// for the map editor's 4x4 sector templates (vehicles 12, weapons and items 50, brushes 300, trees 800 ...), both when
// an object is placed and when a level's entities become editor objects (820F70F0). A campaign level has 16x16
// sectors, so every cap grows by the level's sectors over a template's ([[hook]]s in config/leveleditor.toml).
#include <algorithm>
#include <cstdint>

#include "editor/tools.h"
#include "kernel/kernel.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kTemplateSectors = 16;
constexpr uint32_t kRetrievalStations = 6;

uint32_t Scale() { return uint32_t(fcip::editor::BudgetScale()); }

}  // namespace

int fcip::editor::BudgetScale() {
    if (!g_solo) return 1;
    uint32_t side = R32(SYM_Terrain_SectorsPerSide);
    return int(std::max(side * side / kTemplateSectors, 1u));
}

// a count where its budget is compared, r11: in a template's units, so count < cap * scale stays exact
void BudgetCountR11(PPCContext& ctx, uint8_t*) { ctx.r[11] = uint32_t(ctx.r[11]) / Scale(); }

// the retrieval stations a level loads (820F73E4 compares r10 with 6); r10 then sizes their list, so the compare is
// made again instead
void RetrievalStationsLoaded(PPCContext& ctx, uint8_t*) {
    uint32_t count = uint32_t(ctx.r[10]) / Scale();
    ctx.cr[6] = {count < kRetrievalStations, count > kRetrievalStations, count == kRetrievalStations, ctx.cr[6].so};
}

// a category's cap as its bar shows it, r3 as the call at 820D7394 returns it
void ShownBudgetR3(PPCContext& ctx, uint8_t*) { ctx.r[3] = uint32_t(ctx.r[3]) * Scale(); }
