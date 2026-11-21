#include <filesystem>
#include <string_view>

#include "app/app.h"
#include "common/custom_levels.h"
#include "common/input.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/panel_colors.h"
#include "common/settings.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

// an F10 panel colour as the compiler window's 0xFFRRGGBB
constexpr uint32_t Argb(const ImVec4& c) {
    auto byte = [](float v) { return uint32_t(v * 255.0f + 0.5f); };
    return 0xFF000000u | byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z);
}

// FCIPEditor.exe's windows and message boxes say they are the editor
constexpr bool kEditor = std::string_view(IMAGE_XEX) == "ige.xex";
constexpr const wchar_t* kTitle =
    kEditor ? L"Far Cry Instincts: Predator - XEX Recomp Editor" : L"Far Cry Instincts: Predator - XEX Recomp";

// custom levels live in this exe's custom levels folder, made at start and laid over the disc's Levels; the editor's
// new levels too, in theirs, which the last run's are cleared from
void Loaded() {
    std::error_code ec;
    std::filesystem::create_directories(fcip::CustomLevelsFolder(), ec);
    kernel::MountDiscOverlay("Levels", fcip::CustomLevelsFolder().string());
    if (!kEditor) return;
    std::filesystem::remove_all(fcip::NewLevelsFolder(), ec);
    kernel::MountDiscOverlay("Levels", fcip::NewLevelsFolder().string());
}

void Init() {
    fcip::EnsureDisplay();
    if (fcip::g_editor_launch) fcip::g_editor_launch();
}

bool TalkKey() { return fcip::keys::BindDown(fcip::keys::kPushToTalk); }

}  // namespace

const app::GameInfo app::kGame = {
    .title = kTitle,
    .short_title = kTitle,
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
    .loaded = Loaded,
    .window = fcip::keys::WindowUp,
    .message = fcip::MenuMessage,
    .ui = fcip::DrawUi,
    .pad = fcip::GamePad,
    .rumble = fcip::GameRumble,
    .talk_key = TalkKey,
    .compiler_colours = {
        .background = Argb(fcip::kPanelBg),
        .text = Argb(fcip::kText),
        .dim = Argb(fcip::kDim),
        .bar_empty = Argb(fcip::kButton),
        .bar_full = Argb(fcip::kButtonHovered),
        .lit_text = Argb(fcip::kText),
    },
};
