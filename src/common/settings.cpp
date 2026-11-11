#include "common/settings.h"

#include <algorithm>
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
    s.fov = std::clamp(t["fov"].value_or(s.fov), 45.0f, 140.0f);
    s.fps_counter = t["fps_counter"].value_or(s.fps_counter);
    s.shadow_resolution = std::clamp(t["shadow_resolution"].value_or(s.shadow_resolution), 512u, 8192u);
    for (auto [key, value] : {std::pair{"shadow_distance", &s.shadow_distance}, {"object_distance", &s.object_distance},
                              {"detail_distance", &s.detail_distance}, {"grass_distance", &s.grass_distance},
                              {"vegetation_detail", &s.vegetation_detail}, {"model_detail", &s.model_detail}})
        *value = std::clamp(t[key].value_or(*value), 1u, 32u);
    s.blur = t["aim_and_sprint_blur"].value_or(s.blur);
    s.sensitivity = std::clamp(t["mouse_sensitivity"].value_or(s.sensitivity), 0.02f, 4.0f);
    s.invert_y = t["invert_mouse_y"].value_or(s.invert_y);
    s.mouse_smoothing = t["mouse_smoothing"].value_or(s.mouse_smoothing);
    s.hold_aim = t["hold_to_aim"].value_or(s.hold_aim);
    s.hold_sprint = t["hold_to_sprint"].value_or(s.hold_sprint);
    s.auto_helpers = t["auto_disable_helpers"].value_or(s.auto_helpers);
    s.skip_intros = t["skip_intros"].value_or(s.skip_intros);
    s.invincible = t["invincibility"].value_or(s.invincible);
    keys::Load(table);
}

void SaveSettings(void* table) {
    auto& t = *static_cast<toml::table*>(table);
    const Settings& s = Get();
    t.insert_or_assign("fov", double(s.fov));
    t.insert_or_assign("fps_counter", s.fps_counter);
    t.insert_or_assign("shadow_resolution", int64_t(s.shadow_resolution));
    t.insert_or_assign("shadow_distance", int64_t(s.shadow_distance));
    t.insert_or_assign("object_distance", int64_t(s.object_distance));
    t.insert_or_assign("detail_distance", int64_t(s.detail_distance));
    t.insert_or_assign("grass_distance", int64_t(s.grass_distance));
    t.insert_or_assign("vegetation_detail", int64_t(s.vegetation_detail));
    t.insert_or_assign("model_detail", int64_t(s.model_detail));
    t.insert_or_assign("aim_and_sprint_blur", s.blur);
    t.insert_or_assign("mouse_sensitivity", double(s.sensitivity));
    t.insert_or_assign("invert_mouse_y", s.invert_y);
    t.insert_or_assign("mouse_smoothing", s.mouse_smoothing);
    t.insert_or_assign("hold_to_aim", s.hold_aim);
    t.insert_or_assign("hold_to_sprint", s.hold_sprint);
    t.insert_or_assign("auto_disable_helpers", s.auto_helpers);
    t.insert_or_assign("skip_intros", s.skip_intros);
    t.insert_or_assign("invincibility", s.invincible);
    keys::Save(table);
}

}  // namespace fcip
