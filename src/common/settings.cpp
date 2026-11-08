#include "common/settings.h"

#include <algorithm>

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
    s.blur = t["aim_and_sprint_blur"].value_or(s.blur);
    s.sensitivity = std::clamp(t["mouse_sensitivity"].value_or(s.sensitivity), 0.02f, 4.0f);
    s.invert_y = t["invert_mouse_y"].value_or(s.invert_y);
    s.mouse_smoothing = t["mouse_smoothing"].value_or(s.mouse_smoothing);
    s.hold_aim = t["hold_to_aim"].value_or(s.hold_aim);
    s.hold_sprint = t["hold_to_sprint"].value_or(s.hold_sprint);
    s.walk_speed = std::clamp(t["walk_speed"].value_or(s.walk_speed), 0.1f, 1.0f);
    s.auto_helpers = t["auto_disable_helpers"].value_or(s.auto_helpers);
    keys::Load(table);
}

void SaveSettings(void* table) {
    auto& t = *static_cast<toml::table*>(table);
    const Settings& s = Get();
    t.insert_or_assign("fov", double(s.fov));
    t.insert_or_assign("aim_and_sprint_blur", s.blur);
    t.insert_or_assign("mouse_sensitivity", double(s.sensitivity));
    t.insert_or_assign("invert_mouse_y", s.invert_y);
    t.insert_or_assign("mouse_smoothing", s.mouse_smoothing);
    t.insert_or_assign("hold_to_aim", s.hold_aim);
    t.insert_or_assign("hold_to_sprint", s.hold_sprint);
    t.insert_or_assign("walk_speed", double(s.walk_speed));
    t.insert_or_assign("auto_disable_helpers", s.auto_helpers);
    keys::Save(table);
}

}  // namespace fcip
