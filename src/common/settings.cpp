#include "common/settings.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include <toml++/toml.hpp>

#include "common/keys.h"

namespace fcip {

Settings& Get() {
    static Settings s;
    return s;
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
    s.ambient_occlusion = std::min(t["ambient_occlusion"].value_or(s.ambient_occlusion), 2u);
    s.shadow_resolution = std::min(t["shadow_resolution"].value_or(s.shadow_resolution), 8192u);
    for (auto [key, value] : {std::pair{"shadow_distance", &s.shadow_distance}, {"object_distance", &s.object_distance},
                              {"detail_distance", &s.detail_distance}, {"grass_distance", &s.grass_distance},
                              {"vegetation_detail", &s.vegetation_detail}, {"model_detail", &s.model_detail}})
        *value = std::clamp(t[key].value_or(*value), 1u, 32u);
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
    t.insert_or_assign("ambient_occlusion", int64_t(s.ambient_occlusion));
    t.insert_or_assign("shadow_resolution", int64_t(s.shadow_resolution));
    t.insert_or_assign("shadow_distance", int64_t(s.shadow_distance));
    t.insert_or_assign("object_distance", int64_t(s.object_distance));
    t.insert_or_assign("detail_distance", int64_t(s.detail_distance));
    t.insert_or_assign("grass_distance", int64_t(s.grass_distance));
    t.insert_or_assign("vegetation_detail", int64_t(s.vegetation_detail));
    t.insert_or_assign("model_detail", int64_t(s.model_detail));
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
    keys::Save(table);
}

}  // namespace fcip
