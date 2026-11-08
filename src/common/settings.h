// the game's own settings, beside the runtime's in the settings file
#pragma once
#include <cstdint>

namespace fcip {

struct Settings {
    float fov = 90.0f;  // true horizontal degrees, 45 to 140
    bool fps_counter = false;
    bool blur = true;   // the game's blur while scoped and during feral run
    uint32_t shadow_resolution = 1344;  // sun shadow map side, 1344 is stock
    uint32_t shadow_distance = 1;       // multipliers on the level's own values, 1 is stock
    uint32_t object_distance = 1;
    uint32_t detail_distance = 1;
    uint32_t grass_distance = 1;
    uint32_t vegetation_detail = 1;  // sprite switch distance
    uint32_t model_detail = 1;       // mesh LOD distance
    float sensitivity = 1.0f;      // mouse look, the game's own pad look is 1
    bool invert_y = false;
    bool mouse_smoothing = false;  // the game's pitch lag on the body and weapon
    bool hold_aim = true;          // aim while the key is held, not a toggle
    bool hold_sprint = true;       // feral run while its key is held, not a toggle
    float walk_speed = 0.5f;       // of running
    bool auto_helpers = true;      // the aiming helper off while the keyboard or mouse is in use
    bool skip_intros = true;       // the logo, the branding plate and the intro movie
};

Settings& Get();

// GameInfo load and save: these settings and the binds
void LoadSettings(const void* table);
void SaveSettings(void* table);

}  // namespace fcip
