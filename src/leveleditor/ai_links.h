// the AI's own links between two things a link drag joined (ai_links.cpp): an enemy's or vehicle's anchors and path,
// a spawn spot's generator, an anchor's next anchor and the actions that go to the other
#pragma once
#include "leveleditor/link_edit.h"

namespace fcip::editor {

void DrawAiLinks(const Endpoint& from, const Endpoint& to);  // window thread, in the link window

}  // namespace fcip::editor
