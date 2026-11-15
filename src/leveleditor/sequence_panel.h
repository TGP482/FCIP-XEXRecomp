// the solo level editor's Sequences tab: the level's sequences with a timeline to scrub and preview, their nodes,
// tracks and keys to edit, and the picked sequence's position keys dragged in the view (sequence_panel.cpp)
#pragma once
#include <imgui.h>

#include "leveleditor/view_draw.h"

namespace fcip::editor {

// window thread
void DrawSequencePanel();
bool SequenceMouse(ImVec2 mouse, bool pressed, bool down);  // true while a key drag has the mouse
void DrawSequenceKeys(const View& v);

}  // namespace fcip::editor
