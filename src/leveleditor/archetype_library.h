// the solo level editor's Archetypes tab: the editor's archetype library and the kept mission's EntityLibrary
// (archetype_library.cpp)
#pragma once

struct PPCContext;

namespace fcip::editor {

void DrawArchetypeLibraryPanel();             // window thread
void UpdateArchetypeLibrary(PPCContext& ctx);  // editor thread, each frame: queued edits, then a fresh snapshot after any

}  // namespace fcip::editor
