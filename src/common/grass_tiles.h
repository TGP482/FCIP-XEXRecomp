// the grass tile cache grown past the renderer's 512 entries, for grass drawn further out
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip::grass {

constexpr uint32_t kStockEntries = 512;

enum class Admission { Take, Full, Wait };

// before the budget compare: Take lets the tile through, Wait puts a far miss off a frame, Full leaves the stock test
Admission Admit(PPCContext& ctx, uint32_t renderer, uint32_t tiles, uint32_t params);
// replaces the cache scan: the entry holding this tile's blades, or the least recent one rekeyed; true on a hit
bool Lookup(PPCContext& ctx, uint32_t renderer, uint32_t params, uint32_t& desc);
// the miss path after the blade copy
void Refilled(uint32_t renderer, uint32_t params);

}  // namespace fcip::grass
