// the classes the map editor leaves alone, kept by the solo editor for a campaign level (editor_classes.cpp)
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip::editor {

// editor thread
bool SavesAsEntity(PPCContext& ctx, uint32_t entity);  // one of them with an editor object, which the save writes
void AllowClasses(bool allow);  // the editor's class check says yes while set, as the save writes one entity

}  // namespace fcip::editor
