#include "app/app.h"
#include "common/input.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/panel_colors.h"
#include "common/settings.h"

namespace {

// an F10 panel colour as the compiler window's 0xFFRRGGBB
constexpr uint32_t Argb(const ImVec4& c) {
    auto byte = [](float v) { return uint32_t(v * 255.0f + 0.5f); };
    return 0xFF000000u | byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z);
}

void Init() {
    fcip::EnsureDisplay();
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
    .compiler_colours = {
        .background = Argb(fcip::kPanelBg),
        .text = Argb(fcip::kText),
        .dim = Argb(fcip::kDim),
        .bar_empty = Argb(fcip::kButton),
        .bar_full = Argb(fcip::kButtonHovered),
        .lit_text = Argb(fcip::kText),
    },
};
