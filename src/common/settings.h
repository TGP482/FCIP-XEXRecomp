// the game's own settings, beside the runtime's in the settings file
#pragma once
#include <cstdint>
#include <string>

#include "common/gyro.h"

namespace app {
struct Settings;
}

namespace fcip {

struct Settings {
    std::string monitor;  // its device name, \\.\DISPLAY2; empty the primary
    float fov = 90.0f;  // true horizontal degrees, 45 to 140
    float viewmodel_fov = 90.0f;  // the weapon and arms on the same measure, 90 keeps the game's
    bool fps_counter = false;
    bool hdr_output = false;  // HDR10 on a display with Windows HDR on
    uint32_t colour_blind = 0;  // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    bool smaa = false;          // over the finished frame, HUD included; combines with MSAA
    bool taa = false;           // temporal anti aliasing in place of MSAA, at the render resolution
    bool fsr = false;           // AMD FSR in place of MSAA, the render resolution its input
    uint32_t frame_generation = 0;  // AMD FSR frames generated between each two rendered, 0 off to 3 (4X)
    uint32_t ambient_occlusion = 0;  // 0 off, 1 SSAO, 2 GTAO, 3 RTAO; the world only, under the fog
    bool global_illumination = false;  // ray traced bounce light over the camera's scene, under the fog
    bool volumetric_fog = false;       // height fog in place of the game's, shafts where the sun reaches
    bool volumetric_clouds = false;    // cumulus over the sky box, turning with it and thickening in rain
    bool caustics = false;             // the light the water gathers on what lies under the ocean and the lakes
    uint32_t shadow_resolution = 1344;  // sun shadow map edge, 0 follows the render scale; 1344 is stock
    uint32_t shadow_filter = 0;         // shadow edges, kFilterSharp as the game
    uint32_t ocean_reflection = 0;      // kOcean360 as the game, kOceanXbox the original Xbox game's look
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
    // controller look over the game's own turn, 1 keeps it; while aiming only the aim pair applies
    float pad_sensitivity_x = 1.0f;
    float pad_sensitivity_y = 1.0f;
    float pad_aim_sensitivity_x = 1.0f;
    float pad_aim_sensitivity_y = 1.0f;
    bool pad_invert_x = false;
    bool pad_invert_y = false;
    bool pad_hold_aim = false;     // aim while the right stick is held in, the game toggles on each click
    uint32_t vibration_strength = 100;  // percent of the game's rumble, 10 to 200
    bool skip_intros = false;      // the logo, the branding plate and the intro movie
    gyro::Settings gyro;           // the controller's gyro turns the view, beside the right stick
    bool invincible = false;
    bool noclip = false;  // the noclip key flies the player
    std::string profile;  // the current profile's name, empty until the first one is made
};

Settings& Get();

// FSR or TAA: the camera's pass drawn jittered, with motion vectors
inline bool Temporal() { return Get().fsr || Get().taa; }

// the camera's pass with motion vectors: FSR, TAA or frame generation
inline bool Motion() { return Temporal() || Get().frame_generation; }

// ray traced shadows cover what the game's do, Ultra also what it leaves out: vehicles, dropped weapons, bushes
enum : uint32_t { kFilterSharp, kFilterSoft, kFilterPcss, kFilterRay, kFilterRayUltra };
enum : uint32_t { kOcean360, kOceanXbox };

constexpr uint32_t kMaxWorldDistance = 8;  // past it only the shadow distance grows

// the draw distance for objects, detail, grass, sprite switch and mesh LOD
uint32_t WorldDistance();

// the display mode and size of a fresh settings file: the game's as the runtime has them, FCIPEditor.exe in a window
void DefaultDisplay(app::Settings& s);
// GameInfo init: a missing settings file never reached LoadSettings
void EnsureDisplay();

// GameInfo load and save: these settings and the binds
void LoadSettings(const void* table);
void SaveSettings(void* table);

}  // namespace fcip
