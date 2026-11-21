// default.xex: the original Xbox profile list (P_Profile), each profile saved to profiles\<name>.bin beside the exe
#pragma once

struct PPCContext;

namespace fcip::profiles {

void NoteMainMenu();         // the main menu came up
void Tick(PPCContext& ctx);  // once a menu frame: the first start's CREATE A PROFILE

}  // namespace fcip::profiles
