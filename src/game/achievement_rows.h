// default.xex: the rows of the User page's ACHIEVEMENTS, the current profile's achievements under CAMPAIGN and
// MULTIPLAYER, earned ones first, then RESET ACHIEVEMENTS
#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "common/menu.h"

namespace fcip::achievement_rows {

void Refresh();  // read again from the profile's achievements
std::span<const rows::Row> Rows();
std::string Value(const rows::Row& row);  // a heading's count, as "5 / 16 (31%)"
bool Locked(const rows::Row& row);
uint32_t Image(const rows::Row& row);  // the SPA picture, 0 for none
void Reset();

}  // namespace fcip::achievement_rows
