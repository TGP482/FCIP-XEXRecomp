// the game's own settings, beside the runtime's in the settings file
#pragma once
#include <cstdint>
#include <string>

#include "common/gyro.h"

namespace fcip {

struct Settings {
    std::string monitor;  // its device name, \\.\DISPLAY2; empty the primary
    float fov = 90.0f;  // true horizontal degrees, 45 to 140
    float viewmodel_fov = 90.0f;  // the weapon and arms on the same measure, 90 keeps the game's
    bool fps_counter = false;
    bool hdr_output = false;  // HDR10 on a display with Windows HDR on
    uint32_t colour_blind = 0;  // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    bool smaa = false;          // over the finished frame, HUD included; combines with MSAA
    uint32_t ambient_occlusion = 0;  // 0 off, 1 SSAO, 2 GTAO; the world only, under the fog
    uint32_t shadow_resolution = 1344;  // sun shadow map side, 1344 is stock
    uint32_t shadow_distance = 1;       // multipliers on the level's own values, 1 is stock
    uint32_t object_distance = 1;
    uint32_t detail_distance = 1;
    uint32_t grass_distance = 1;
    uint32_t vegetation_detail = 1;  // sprite switch distance
    uint32_t model_detail = 1;       // mesh LOD distance
    bool blur = true;                // the game's blur while scoped and during feral run
    // mouse look, the game's own pad look is 1; while aiming only the aim pair applies
    float sensitivity_x = 1.0f;
    float sensitivity_y = 1.0f;
    float aim_sensitivity_x = 1.0f;
    float aim_sensitivity_y = 1.0f;
    bool invert_x = false;
    bool invert_y = false;
    bool mouse_smoothing = false;  // the game's pitch lag on the body and weapon
    bool hold_aim = true;          // aim while the key is held, not a toggle
    bool hold_sprint = true;       // feral run while its key is held, not a toggle
    bool auto_helpers = true;      // the aiming helper off while the keyboard or mouse is in use
    bool skip_intros = true;       // the logo, the branding plate and the intro movie
    gyro::Settings gyro;           // the controller's gyro turns the view, beside the right stick
    bool invincible = false;
};

Settings& Get();

// GameInfo load and save: these settings and the binds
void LoadSettings(const void* table);
void SaveSettings(void* table);

}  // namespace fcip
