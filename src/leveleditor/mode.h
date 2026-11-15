// which editor FCIPEditor.exe runs, picked at launch (launch.cpp)
#pragma once

namespace fcip::editor {

// the solo level editor's hooks pass straight through to the map editor without it
inline bool g_solo = false;

void ForgetPlayTest();  // a level starts (simulation.cpp)

}  // namespace fcip::editor
