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
    bool fsr = false;           // AMD FSR in place of MSAA, the render resolution its input
    bool frame_generation = false;  // AMD FSR frame generation, with FSR only
    uint32_t ambient_occlusion = 0;  // 0 off, 1 SSAO, 2 GTAO, 3 RTAO; the world only, under the fog
    uint32_t shadow_resolution = 1344;  // sun shadow map edge, 0 follows the render scale; 1344 is stock
    uint32_t shadow_filter = 0;         // shadow edges, kFilterSharp as the game
    uint32_t draw_distance = 1;         // multiplier on the level's own distances, 1 is stock; shadows take all of it
    bool blur = true;                // the game's blur while scoped and during feral run
    // mouse look, the game's own pad look is 1; while aiming only the aim pair applies
    float sensitivity_x = 1.0f;
    float sensitivity_y = 1.0f;
    float aim_sensitivity_x = 1.0f;
    float aim_sensitivity_y = 1.0f;
    bool invert_x = false;
    bool invert_y = false;
    bool hold_aim = true;          // aim while the key is held, not a toggle
    bool hold_sprint = true;       // feral run while its key is held, not a toggle
    bool skip_intros = true;       // the logo, the branding plate and the intro movie
    gyro::Settings gyro;           // the controller's gyro turns the view, beside the right stick
    bool invincible = false;
    bool noclip = false;  // the noclip key flies the player
};

Settings& Get();

enum : uint32_t { kFilterSharp, kFilterSoft, kFilterPcss, kFilterRay };

constexpr uint32_t kMaxWorldDistance = 8;  // past it only the shadow distance grows

// the draw distance for objects, detail, grass, sprite switch and mesh LOD
uint32_t WorldDistance();

// GameInfo load and save: these settings and the binds
void LoadSettings(const void* table);
void SaveSettings(void* table);

}  // namespace fcip
