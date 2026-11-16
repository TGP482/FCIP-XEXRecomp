// the solo level editor's Equipment tab: the kept mission's equip packs, preloaded weapons and the player's
// maximum ammo (equipment.cpp)
#pragma once

struct PPCContext;

namespace fcip::editor {

void DrawEquipmentPanel();             // window thread
void UpdateEquipment(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any

}  // namespace fcip::editor
