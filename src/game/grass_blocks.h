// grass past the stock radius drawn as blocks of tiles, one draw a block
#pragma once
#include <cstdint>

namespace fcip::grass_blocks {

// the tile params the renderer is adding a block from, for the cache hooks
bool Owns(uint32_t params);
// that block's entry, the draw's description at +4
uint32_t Desc();

}  // namespace fcip::grass_blocks
