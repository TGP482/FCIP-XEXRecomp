// default.xex: the SPA's achievement pictures as menu textures
#pragma once
#include <cstdint>
#include <string>

struct PPCContext;

namespace fcip::achievement_icons {

// the pack holding them, written and opened once on the menu thread; false when it could not be
bool Ensure(PPCContext& ctx);
// the path a menu sprite loads a picture from
std::string Path(uint32_t image);

}  // namespace fcip::achievement_icons
