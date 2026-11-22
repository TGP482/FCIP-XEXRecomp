// default.xex: the rows of the User page's ACHIEVEMENTS, the current profile's achievements under CAMPAIGN and
// MULTIPLAYER, earned ones first, then RESET ACHIEVEMENTS
#pragma once
#include <span>
#include <string>

#include "common/menu.h"

namespace fcip::achievement_rows {

void Refresh();  // read again from the profile's achievements
std::span<const rows::Row> Rows();
std::string Value(const rows::Row& row);  // a heading's count, as "5 / 16 (31%)"
bool Locked(const rows::Row& row);
void Reset();

}  // namespace fcip::achievement_rows
