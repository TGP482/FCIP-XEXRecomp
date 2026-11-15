#include "common/settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/keys.h"
#include "image_symbols.h"

namespace fcip {

namespace {

// FCIPEditor.exe keeps its own display mode and size beside the game's
constexpr bool kEditor = std::string_view(IMAGE_XEX) == "ige.xex";
constexpr const char* kModeNames[] = {"fullscreen", "borderless", "windowed"};

struct DisplayKeys {
    const char* mode;
    const char* width;
    const char* height;
};
constexpr DisplayKeys kGameKeys{"display_mode", "width", "height"};
constexpr DisplayKeys kEditorKeys{"editor_display_mode", "editor_width", "editor_height"};
constexpr DisplayKeys kOtherKeys = kEditor ? kGameKeys : kEditorKeys;

// the other executable's display keys, written back as they were
struct OtherDisplay {
    std::optional<std::string> mode;
    std::optional<int64_t> width;
    std::optional<int64_t> height;
};
OtherDisplay g_other;
bool g_display_loaded = false;

// the largest mode of the primary display within three quarters of its desktop
app::Mode EditorWindowSize() {
    app::Mode desktop = app::Desktop();
    app::Mode best{1280, 720};
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        bool fits = dm.dmPelsWidth * 4 <= desktop.width * 3 && dm.dmPelsHeight * 4 <= desktop.height * 3;
        bool larger = dm.dmPelsWidth * dm.dmPelsHeight > best.width * best.height;
        if (dm.dmBitsPerPel >= 32 && fits && larger) best = {dm.dmPelsWidth, dm.dmPelsHeight};
    }
    return best;
}

void LoadDisplay(const toml::table& t) {
    g_display_loaded = true;
    g_other.mode = t[kOtherKeys.mode].value<std::string>();
    g_other.width = t[kOtherKeys.width].value<int64_t>();
    g_other.height = t[kOtherKeys.height].value<int64_t>();
    if (!kEditor) return;
    app::Settings& s = app::Get();
    DefaultDisplay(s);
    std::string mode = t[kEditorKeys.mode].value_or(std::string(kModeNames[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < std::size(kModeNames); i++) {
        if (mode == kModeNames[i]) s.display_mode = app::DisplayMode(i);
    }
    s.width = t[kEditorKeys.width].value_or(s.width);
    s.height = t[kEditorKeys.height].value_or(s.height);
}

// the runtime wrote this executable's display under the game's keys
void SaveDisplay(toml::table& t) {
    if (kEditor) {
        const app::Settings& s = app::Get();
        t.insert_or_assign(kEditorKeys.mode, kModeNames[uint32_t(s.display_mode)]);
        t.insert_or_assign(kEditorKeys.width, int64_t(s.width));
        t.insert_or_assign(kEditorKeys.height, int64_t(s.height));
        for (const char* key : {kGameKeys.mode, kGameKeys.width, kGameKeys.height}) t.erase(key);
    }
    if (g_other.mode) t.insert_or_assign(kOtherKeys.mode, *g_other.mode);
    if (g_other.width) t.insert_or_assign(kOtherKeys.width, *g_other.width);
    if (g_other.height) t.insert_or_assign(kOtherKeys.height, *g_other.height);
}

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

uint32_t WorldDistance() { return std::min(Get().draw_distance, kMaxWorldDistance); }

void DefaultDisplay(app::Settings& s) {
    const app::Settings def;
    s.display_mode = kEditor ? app::DisplayMode::Windowed : def.display_mode;
    app::Mode size = kEditor ? EditorWindowSize() : app::Mode{def.width, def.height};
    s.width = size.width;
    s.height = size.height;
}

void EnsureDisplay() {
    if (!g_display_loaded) LoadDisplay(toml::table{});
}

void LoadSettings(const void* table) {
    auto& t = *static_cast<const toml::table*>(table);
    Settings& s = Get();
    s.monitor = t["monitor"].value_or(s.monitor);
    s.fov = std::clamp(t["fov"].value_or(s.fov), 45.0f, 140.0f);
    s.viewmodel_fov = std::clamp(t["viewmodel_fov"].value_or(s.viewmodel_fov), 45.0f, 140.0f);
    s.fps_counter = t["fps_counter"].value_or(s.fps_counter);
    s.hdr_output = t["hdr_output"].value_or(s.hdr_output);
    s.colour_blind = std::min(t["colour_blind"].value_or(s.colour_blind), 3u);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.fsr = t["fsr"].value_or(s.fsr);
    s.frame_generation = t["frame_generation"].value_or(s.frame_generation);
    s.ambient_occlusion = std::min(t["ambient_occlusion"].value_or(s.ambient_occlusion), 3u);
    s.global_illumination = t["global_illumination"].value_or(s.global_illumination);
    s.volumetric_fog = t["volumetric_fog"].value_or(s.volumetric_fog);
    s.shadow_resolution = std::min(t["shadow_resolution"].value_or(s.shadow_resolution), 8192u);
    s.shadow_filter = std::min(t["shadow_filter"].value_or(s.shadow_filter), uint32_t(kFilterRay));
    s.ocean_reflection = std::min(t["ocean_reflection"].value_or(s.ocean_reflection), uint32_t(kOceanXbox));
    s.draw_distance = std::clamp(t["draw_distance"].value_or(s.draw_distance), 1u, 32u);
    s.blur = t["aim_and_sprint_blur"].value_or(s.blur);
    // a file from before the split keeps its one sensitivity for both axes
    float old = t["mouse_sensitivity"].value_or(1.0f);
    for (auto [key, value] : {std::pair{"mouse_sensitivity_x", &s.sensitivity_x},
                              {"mouse_sensitivity_y", &s.sensitivity_y}, {"aim_sensitivity_x", &s.aim_sensitivity_x},
                              {"aim_sensitivity_y", &s.aim_sensitivity_y}})
        *value = std::clamp(t[key].value_or(old), 0.02f, 4.0f);
    s.invert_x = t["invert_mouse_x"].value_or(s.invert_x);
    s.invert_y = t["invert_mouse_y"].value_or(s.invert_y);
    s.hold_aim = t["hold_to_aim"].value_or(s.hold_aim);
    s.hold_sprint = t["hold_to_sprint"].value_or(s.hold_sprint);
    s.skip_intros = t["skip_intros"].value_or(s.skip_intros);
    s.gyro.enabled = t["gyro_aiming"].value_or(s.gyro.enabled);
    s.gyro.sensitivity = std::clamp(t["gyro_sensitivity"].value_or(s.gyro.sensitivity), 0.1f, 5.0f);
    s.gyro.axis = std::min(t["gyro_axis"].value_or(s.gyro.axis), uint32_t(gyro::kWorld));
    s.gyro.button = std::min(t["gyro_button"].value_or(s.gyro.button), uint32_t(std::size(gyro::kButtonNames) - 1));
    s.gyro.button_mode = std::min(t["gyro_button_mode"].value_or(s.gyro.button_mode), uint32_t(gyro::kToggle));
    s.invincible = t["invincibility"].value_or(s.invincible);
    s.noclip = t["noclip"].value_or(s.noclip);
    LoadDisplay(t);
    keys::Load(table);
}

void SaveSettings(void* table) {
    auto& t = *static_cast<toml::table*>(table);
    const Settings& s = Get();
    t.insert_or_assign("monitor", s.monitor);
    t.insert_or_assign("fov", double(s.fov));
    t.insert_or_assign("viewmodel_fov", double(s.viewmodel_fov));
    t.insert_or_assign("fps_counter", s.fps_counter);
    t.insert_or_assign("hdr_output", s.hdr_output);
    t.insert_or_assign("colour_blind", int64_t(s.colour_blind));
    t.insert_or_assign("smaa", s.smaa);
    t.insert_or_assign("fsr", s.fsr);
    t.insert_or_assign("frame_generation", s.frame_generation);
    t.insert_or_assign("ambient_occlusion", int64_t(s.ambient_occlusion));
    t.insert_or_assign("global_illumination", s.global_illumination);
    t.insert_or_assign("volumetric_fog", s.volumetric_fog);
    t.insert_or_assign("shadow_resolution", int64_t(s.shadow_resolution));
    t.insert_or_assign("shadow_filter", int64_t(s.shadow_filter));
    t.insert_or_assign("ocean_reflection", int64_t(s.ocean_reflection));
    t.insert_or_assign("draw_distance", int64_t(s.draw_distance));
    // the six distances before they became one
    for (const char* key : {"shadow_distance", "object_distance", "detail_distance", "grass_distance",
                            "vegetation_detail", "model_detail"})
        t.erase(key);
    t.insert_or_assign("aim_and_sprint_blur", s.blur);
    t.erase("mouse_sensitivity");
    t.erase("mouse_smoothing");
    t.erase("auto_disable_helpers");
    t.insert_or_assign("mouse_sensitivity_x", double(s.sensitivity_x));
    t.insert_or_assign("mouse_sensitivity_y", double(s.sensitivity_y));
    t.insert_or_assign("aim_sensitivity_x", double(s.aim_sensitivity_x));
    t.insert_or_assign("aim_sensitivity_y", double(s.aim_sensitivity_y));
    t.insert_or_assign("invert_mouse_x", s.invert_x);
    t.insert_or_assign("invert_mouse_y", s.invert_y);
    t.insert_or_assign("hold_to_aim", s.hold_aim);
    t.insert_or_assign("hold_to_sprint", s.hold_sprint);
    t.insert_or_assign("skip_intros", s.skip_intros);
    t.insert_or_assign("gyro_aiming", s.gyro.enabled);
    t.insert_or_assign("gyro_sensitivity", double(s.gyro.sensitivity));
    t.insert_or_assign("gyro_axis", int64_t(s.gyro.axis));
    t.insert_or_assign("gyro_button", int64_t(s.gyro.button));
    t.insert_or_assign("gyro_button_mode", int64_t(s.gyro.button_mode));
    t.insert_or_assign("invincibility", s.invincible);
    t.insert_or_assign("noclip", s.noclip);
    SaveDisplay(t);
    keys::Save(table);
}

}  // namespace fcip
