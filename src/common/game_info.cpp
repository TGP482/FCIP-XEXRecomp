#include "app/app.h"
#include "common/input.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/settings.h"

namespace {

void Init() {
    if (fcip::g_editor_launch) fcip::g_editor_launch();
}

}  // namespace

const app::GameInfo app::kGame = {
    .title = L"Far Cry Instincts: Predator - XEX Recomp",
    .short_title = L"Far Cry Instincts: Predator - XEX Recomp",
    .name = L"FCIP",
    .log = "fcip",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xex with its folders beside it.",
    .images = {{"default.xex", "FCIP"}, {"ige.xex", "FCIPEditor"}},
    .options = {},
    .load = fcip::LoadSettings,
    .save = fcip::SaveSettings,
    .init = Init,
    .loaded = nullptr,
    .window = fcip::keys::WindowUp,
    .message = fcip::MenuMessage,
    .ui = fcip::DrawUi,
    .pad = fcip::GamePad,
    .rumble = fcip::GameRumble,
    .compiler_colours = {},
};
