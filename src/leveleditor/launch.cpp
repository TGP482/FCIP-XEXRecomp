// FCIPEditor.exe's launch question: the map editor as the game ships it, or with the solo level editor on top
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>

#include "app/app.h"
#include "common/menu.h"
#include "core/log.h"
#include "leveleditor/mode.h"

namespace {

constexpr int kSolo = 1;

// the game opens the map editor from its menu, behind its last frame: that launch asks nothing
bool FromTheGame() { return std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr; }

void AskEditor() {
    if (FromTheGame()) return;
    int pick = app::Choose(L"Which editor do you want to open?", {L"Multiplayer Map Editor", L"Solo Level Editor"});
    if (pick < 0) {
        logx::Flush();
        ExitProcess(0);
    }
    fcip::editor::g_solo = pick == kSolo;
    LOGR("editor: %s", fcip::editor::g_solo ? "solo level editor" : "multiplayer map editor");
}

const bool g_registered = (fcip::g_editor_launch = AskEditor, true);

}  // namespace
