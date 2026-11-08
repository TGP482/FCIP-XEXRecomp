#include "app/app.h"

const app::GameInfo app::kGame = {
    .title = L"Far Cry Instincts: Predator - XEX Recomp",
    .short_title = L"Far Cry Instincts: Predator - XEX Recomp",
    .name = L"FCIP",
    .log = "fcip",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xex with its folders beside it.",
    .images = {{"default.xex", "FCIP"}},
    .options = {},
    .load = nullptr,
    .save = nullptr,
    .init = nullptr,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .pad = nullptr,
    .rumble = nullptr,
    .compiler_colours = {},
};
