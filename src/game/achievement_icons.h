// default.xex: the SPA's achievement pictures as menu and HUD textures
#pragma once
#include <cstdint>
#include <string>

struct PPCContext;

namespace fcip::achievement_icons {

// the pack holding them, written and opened the first time; false when it could not be
bool Ensure(PPCContext& ctx);
// the path a menu sprite loads a picture from
std::string Path(uint32_t image);
// the picture's sprite in a UI's sprites, added the first time; 0 when it could not be
uint32_t Sprite(PPCContext& ctx, uint32_t sprites, uint32_t image);

}  // namespace fcip::achievement_icons
