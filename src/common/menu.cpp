// the options panel on F10
#include "common/menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>

#include "app/app.h"
#include "audio/xaudio2.h"
#include "common/gyro.h"
#include "common/input.h"
#include "common/keys.h"
#include "common/monitor.h"
#include "common/panel_colors.h"
#include "common/settings.h"
#include "core/input.h"
#include "d3d/fsr.h"
#include "d3d/occlusion.h"

namespace fcip {

namespace {

using rows::Kind;
using rows::Row;

std::atomic<bool> g_open{false};
std::atomic<uint32_t> g_revision{0};

constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"OFF", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2X MSAA", "4X MSAA", "8X MSAA"};
const char* const kDumpFormatNames[] = {"PNG", "DDS"};
const char* const kColourBlindNames[] = {"OFF", "PROTANOPIA", "DEUTERANOPIA", "TRITANOPIA"};
const char* const kAmbientOcclusionNames[] = {"OFF", "SSAO", "GTAO", "RTAO"};
const char* const kShadowFilterNames[] = {"SHARP (DEFAULT)", "SOFT"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, app::kFpsCapRefresh, 0};
constexpr uint32_t kShadowSizes[] = {0, 1024, 1344, 2048, 3072, 4096, 6144, 8192};
constexpr uint32_t kDistances[] = {1, 2, 4, 6, 8, 12, 16, 32};

// rows::Choices reads a row's values by picking each index in turn; -1 steps as usual
int g_pick = -1;
int g_picks = 0;
int g_picked = 0;

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    if (g_pick >= 0) {
        g_picks = count;
        g_picked = index;
        return std::min(g_pick, count - 1);
    }
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = StepIndex(on, step, 2) != 0;
}

enum RowId {
    kMonitor, kDisplayMode, kResolution, kRenderResolution, kVsync, kMaxFps, kAniso, kSensitivityX, kSensitivityY,
    kAimSensitivityX, kAimSensitivityY, kInvertX, kInvertY, kHoldAim, kHoldSprint, kSkipIntros, kFov, kViewmodelFov,
    kFpsCounter, kHdrOutput, kShadowResolution, kShadowFilter,
    kBlur, kDrawDistance,
    kInvincible, kNoclip, kTextureReplacement, kTextureDumping, kDumpFormat, kGyroAiming, kGyroSensitivity, kGyroAxis,
    kGyroButton, kGyroButtonMode, kGyroCalibrate, kColourBlind, kAntiAliasing, kFrameGeneration, kSmaa, kAmbientOcclusion,
    kAudioOutput
};

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY", "Monitor, resolution, field of view and frame rate"},
    {Kind::Value, kMonitor, "MONITOR", "Picks the monitor the game shows on, by Windows number and desktop resolution"},
    {Kind::Value, kDisplayMode, "DISPLAY MODE", "Switch between fullscreen, borderless and windowed"},
    {Kind::Value, kResolution, "RESOLUTION", "Every mode your monitor reports, defaulting to your desktop resolution"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION",
     "Renders the game at 20% to 200% of the output resolution, independent of the window size, or 33% to 100% upscaled with FSR on"},
    {Kind::Value, kFov, "FIELD OF VIEW", "Sets the horizontal field of view during gameplay, cutscenes keep their own"},
    {Kind::Value, kViewmodelFov, "VIEWMODEL FIELD OF VIEW", "Sets the field of view of the weapon and arms"},
    {Kind::Value, kVsync, "VERTICAL SYNC", "Reduces screen tearing"},
    {Kind::Value, kHdrOutput, "HDR OUTPUT", "Outputs HDR10 on a display with HDR turned on in Windows"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE",
     "Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap"},
    {Kind::Value, kFpsCounter, "FPS COUNTER", "Displays the frame rate on screen"},
    {Kind::Value, kColourBlind, "COLOUR BLIND MODE", "Adjusts the colours for protanopia, deuteranopia or tritanopia"},
    {Kind::Heading, -1, "GRAPHICS", "Antialiasing, ambient occlusion, texture filtering, shadows and draw distance"},
    {Kind::Value, kAmbientOcclusion, "AMBIENT OCCLUSION",
     "Shades the world's creases and corners, SSAO, the more accurate GTAO or ray traced RTAO, characters and fog left alone"},
    {Kind::Value, kAntiAliasing, "ANTI ALIASING",
     "Smooths jagged edges with 2X, 4X or 8X MSAA in place of the game's own 2X, or AMD FSR 3 from the render resolution, sharpened by AMD CAS"},
    {Kind::Value, kFrameGeneration, "FRAME GENERATION",
     "Generates a frame between each two rendered ones with AMD FSR Frame Generation, with FSR on"},
    {Kind::Value, kSmaa, "SMAA", "Smooths jagged edges across the whole frame, foliage and fences included"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING", "Forces the selected anisotropic filtering level on all textures"},
    {Kind::Value, kShadowResolution, "SHADOW RESOLUTION", "Sun shadow map resolution from 1024x1024 to 8192x8192, 1344x1344 is stock, automatic follows the render resolution"},
    {Kind::Value, kShadowFilter, "SHADOW FILTERING",
     "Sharp keeps the game's own shadow edges, soft blurs them by the same amount at every resolution"},
    {Kind::Value, kDrawDistance, "DRAW DISTANCE",
     "Scales how far objects, grass and vegetation are drawn up to 8x and the area casting sun shadows up to 32x"},
    {Kind::Value, kBlur, "AIM AND SPRINT BLUR", "The blur the game adds while aiming down a scope and during feral run"},
    {Kind::Heading, -1, "GENERAL", "Intros and audio output"},
    {Kind::Value, kSkipIntros, "SKIP INTROS", "Skips the Ubisoft logo, the branding plate and the intro movie"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT",
     "Mono, stereo or 5.1 surround, automatic follows the speakers set up in Windows"},
    {Kind::Heading, -1, "GYRO", "Gyro aiming for controllers with motion sensors"},
    {Kind::Value, kGyroAiming, "GYRO AIMING", "Turns the view with the controller's gyro, beside the right stick"},
    {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY", "Degrees the view turns for each degree the controller turns"},
    {Kind::Value, kGyroAxis, "GYRO AXIS", "Turns with the controller's yaw, its roll, or about the real world's up"},
    {Kind::Value, kGyroButton, "GYRO BUTTON", "A button the game leaves free that switches the gyro"},
    {Kind::Value, kGyroButtonMode, "GYRO BUTTON MODE", "Whether the gyro button turns the gyro off, on or flips it"},
    {Kind::Value, kGyroCalibrate, "CALIBRATE GYRO", "Lay the controller still while its drift is measured"},
    {Kind::Heading, -1, "TEXTURES", "Texture replacement and dumping"},
    {Kind::Value, kTextureReplacement, "TEXTURE REPLACEMENT",
     "Replaces the game's textures with the images in the textures\\replacement folder beside the exe"},
    {Kind::Value, kTextureDumping, "TEXTURE DUMPING",
     "Saves every texture the game uses into the textures\\dumps folder beside the exe as you play"},
    {Kind::Value, kDumpFormat, "DUMP FORMAT", "PNG, or DDS which keeps the original compression and mipmaps"},
    {Kind::Defaults, -1, "RESTORE DEFAULTS", "Puts every setting on this page back to its default"},
};

struct Level {
    const char* folder;
    const char* label;
};

// the story levels, the folders the game's LOAD A MAP list scans
constexpr Level kLevels[] = {
    {"00_training", "00 TRAINING"}, {"01_beach", "01 BEACH"}, {"02_native", "02 NATIVE"}, {"03_river", "03 RIVER"},
    {"04_rain_forest", "04 RAIN FOREST"}, {"05_mines", "05 MINES"}, {"06_cliffs", "06 CLIFFS"},
    {"07_sub-aquatic_facility", "07 SUB-AQUATIC FACILITY"}, {"10_dead_marshes", "10 DEAD MARSHES"},
    {"11_wasteland", "11 WASTELAND"}, {"13_black_forest", "13 BLACK FOREST"}, {"21_dunea", "21 DUNE A"},
    {"22_duneb", "22 DUNE B"}, {"23_dunec", "23 DUNE C"}, {"24_shantyday", "24 SHANTY DAY"},
    {"26_shantynight", "26 SHANTY NIGHT"}, {"28_jungle", "28 JUNGLE"}, {"29_forest", "29 FOREST"},
    {"30_mountain", "30 MOUNTAIN"},
};

const std::vector<Row>& DebugRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {
            {Kind::Heading, -1, "CHEATS", nullptr},
            {Kind::Value, kInvincible, "INVINCIBILITY", "Makes the player take no damage"},
            {Kind::Value, kNoclip, "NOCLIP",
             "Lets the noclip key fly the player through walls, jump and crouch rise and sink"},
            {Kind::Bind, keys::kNoclip, keys::Label(keys::kNoclip), "Accept rebinds the key, Delete unbinds it"},
            {Kind::Heading, -1, "LOAD LEVEL", nullptr},
        };
        for (int i = 0; i < int(std::size(kLevels)); i++) out.push_back({Kind::Level, i, kLevels[i].label, nullptr});
        out.push_back({Kind::Defaults, -1, "RESTORE DEFAULTS", "Turns invincibility and noclip off"});
        return out;
    }();
    return list;
}

const std::vector<Row>& KeyboardRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {
            {Kind::Heading, -1, "MOUSE", "Mouse sensitivity, inversion and hold or toggle"},
            {Kind::Value, kSensitivityX, "HORIZONTAL SENSITIVITY", "Adjusts horizontal mouse look sensitivity"},
            {Kind::Value, kSensitivityY, "VERTICAL SENSITIVITY", "Adjusts vertical mouse look sensitivity"},
            {Kind::Value, kAimSensitivityX, "AIM HORIZONTAL SENSITIVITY",
             "Adjusts horizontal mouse look sensitivity while aiming"},
            {Kind::Value, kAimSensitivityY, "AIM VERTICAL SENSITIVITY",
             "Adjusts vertical mouse look sensitivity while aiming"},
            {Kind::Value, kInvertX, "INVERT X-AXIS", "Inverts the horizontal mouse look axis"},
            {Kind::Value, kInvertY, "INVERT Y-AXIS", "Inverts the vertical mouse look axis"},
            {Kind::Value, kHoldAim, "HOLD TO AIM", "Aim only while the aim key is held instead of toggling"},
            {Kind::Value, kHoldSprint, "HOLD TO SPRINT", "Feral run only while the feral run key is held instead of toggling"},
            {Kind::Heading, -1, "KEYBOARD", "Key bindings"},
        };
        for (int i = 0; i < keys::kActionCount; i++) {
            if (i == keys::kNoclip) continue;  // on the debug page
            out.push_back({Kind::Bind, i, keys::Label(i), "Accept rebinds the key, Delete unbinds it"});
        }
        out.push_back({Kind::Defaults, -1, "RESTORE DEFAULTS", "Puts the mouse settings and every key back to its default"});
        return out;
    }();
    return list;
}

bool DisplayRow(int id) { return id == kMonitor || id == kDisplayMode || id == kResolution; }

std::string g_display_monitor;  // the held monitor, beside the held display settings

// the monitor setting beside s: the held one beside the held display settings
std::string& MonitorSetting(const app::Settings& s) {
    if (&s == &app::Get()) return Get().monitor;
    return g_display_monitor;
}

// the setting's monitor in the list, the primary's index when it is not connected
int MonitorIndex(const std::vector<Display>& list, const std::string& monitor) {
    int primary = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].device == monitor) return int(i);
        if (list[i].primary) primary = int(i);
    }
    return primary;
}

// the monitor's modes smallest first, so Left steps down; the primary's for empty
std::vector<app::Mode> Modes(const std::string& monitor) {
    std::vector<app::Mode> out;
    std::wstring device(monitor.begin(), monitor.end());
    const wchar_t* name = device.empty() ? nullptr : device.c_str();
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(name, i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
        app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        auto same = [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; };
        if (std::none_of(out.begin(), out.end(), same)) out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const app::Mode& a, const app::Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return out;
}

// the mode shown on the setting's monitor, stepped first; its desktop mode is kept as 0, which follows the desktop
app::Mode StepResolution(app::Settings& s, int step) {
    static std::map<std::string, std::vector<app::Mode>> cache;
    std::vector<Display> monitors = Displays();
    std::string device;  // the primary when the setting's monitor is not connected
    app::Mode desktop = app::Desktop();
    if (!monitors.empty()) {
        const Display& d = monitors[size_t(MonitorIndex(monitors, MonitorSetting(s)))];
        device = d.device;
        desktop = d.desktop;
    }
    auto [it, fresh] = cache.try_emplace(device);
    if (fresh) it->second = Modes(device);
    const std::vector<app::Mode>& modes = it->second;
    app::Mode m = s.width && s.height ? app::Mode{s.width, s.height} : desktop;
    int i = 0;
    for (size_t k = 0; k < modes.size(); k++) {
        if (modes[k].width == m.width && modes[k].height == m.height) i = int(k);
    }
    if (step && !modes.empty()) {
        m = modes[size_t(StepIndex(i, step, int(modes.size())))];
        bool native = m.width == desktop.width && m.height == desktop.height;
        s.width = native ? 0 : m.width;
        s.height = native ? 0 : m.height;
    }
    return m;
}

// the monitor shown, stepped first; another monitor starts on its own desktop mode
void StepMonitor(int step, app::Settings& s, char (&text)[64]) {
    std::vector<Display> list = Displays();
    if (list.empty()) {
        std::snprintf(text, sizeof(text), "PRIMARY");
        return;
    }
    int current = MonitorIndex(list, MonitorSetting(s));
    int i = StepIndex(current, step, int(list.size()));
    const Display& d = list[size_t(i)];
    if (step && i != current) {
        MonitorSetting(s) = d.device;
        s.width = 0;
        s.height = 0;
    }
    const char* format = d.primary ? "%d: %ux%u (PRIMARY)" : "%d: %ux%u";
    std::snprintf(text, sizeof(text), format, d.number, d.desktop.width, d.desktop.height);
}

bool* ToggleFlag(int id) {
    Settings& g = Get();
    switch (id) {
        case kInvertX: return &g.invert_x;
        case kInvertY: return &g.invert_y;
        case kHoldAim: return &g.hold_aim;
        case kHoldSprint: return &g.hold_sprint;
        case kSkipIntros: return &g.skip_intros;
        case kFpsCounter: return &g.fps_counter;
        case kHdrOutput: return &g.hdr_output;
        case kSmaa: return &g.smaa;
        case kGyroAiming: return &g.gyro.enabled;
        case kBlur: return &g.blur;
        case kInvincible: return &g.invincible;
        case kNoclip: return &g.noclip;
        default: return nullptr;
    }
}

float* SensitivityField(int id) {
    Settings& g = Get();
    switch (id) {
        case kSensitivityX: return &g.sensitivity_x;
        case kSensitivityY: return &g.sensitivity_y;
        case kAimSensitivityX: return &g.aim_sensitivity_x;
        case kAimSensitivityY: return &g.aim_sensitivity_y;
        default: return nullptr;
    }
}

// one of names, stepped first
template <size_t N>
void StepName(uint32_t& value, const char* const (&names)[N], int step, char (&text)[64]) {
    value = uint32_t(StepIndex(int(std::min<size_t>(value, N - 1)), step, int(N)));
    std::snprintf(text, sizeof(text), "%s", names[value]);
}

// with FSR, its range in fives and its modes' percentages, named
constexpr uint32_t kFsrPercents[] = {33, 35, 40, 45, 50, 55, 59, 60, 65, 67, 70, 75, 80, 85, 90, 95, 100};

void StepRenderResolution(app::Settings& s, int step, char (&text)[64]) {
    uint32_t percent = s.render_percent;
    if (Get().fsr) {
        percent = kFsrPercents[StepTable(kFsrPercents, s.render_percent, step)];
        for (const fsr::ModePoint& m : fsr::kModes) {
            if (m.percent != percent) continue;
            std::snprintf(text, sizeof(text), "%u%% %s", percent, m.name);
            return;
        }
    } else if (step) {
        s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
        percent = s.render_percent;
    }
    app::Mode out = StepResolution(s, 0);
    std::snprintf(text, sizeof(text), "%u%% (%ux%u)", percent, out.width * percent / 100, out.height * percent / 100);
}

// MSAA's sample counts, then FSR
void StepAntiAliasing(app::Settings& s, int step, char (&text)[64]) {
    constexpr int kMsaaCount = int(std::size(kMsaaValues));
    int i = Get().fsr ? kMsaaCount : NearestIndex(kMsaaValues, kMsaaCount, s.msaa);
    if (step) {
        i = StepIndex(i, step, kMsaaCount + 1);
        Get().fsr = i == kMsaaCount;
        s.msaa = Get().fsr ? 0 : kMsaaValues[i];
    }
    if (i < kMsaaCount) std::snprintf(text, sizeof(text), "%s", kMsaaNames[i]);
    else std::snprintf(text, sizeof(text), "FSR3%s", fsr::StatusNote(fsr::UpscalingStatus()));
}

void StepFrameGeneration(int step, char (&text)[64]) {
    bool& on = Get().frame_generation;
    if (step) on = !on;
    std::snprintf(text, sizeof(text), "%s%s", on ? "ON" : "OFF", fsr::StatusNote(fsr::GenerationStatus()));
}

void StepGyroRow(int id, int step, char (&text)[64]) {
    gyro::Settings& g = Get().gyro;
    switch (id) {
        case kGyroSensitivity: {
            // 0.1 to 5.0 in steps of 0.1
            int tenths = StepRange(int(std::lround(g.sensitivity * 10.0f)), 1, 50, 1, step);
            if (step) g.sensitivity = float(tenths) / 10.0f;
            std::snprintf(text, sizeof(text), "%.1f", float(tenths) / 10.0f);
            break;
        }
        case kGyroAxis: StepName(g.axis, gyro::kAxisNames, step, text); break;
        case kGyroButton: StepName(g.button, gyro::kButtonNames, step, text); break;
        case kGyroButtonMode: StepName(g.button_mode, gyro::kButtonModeNames, step, text); break;
        case kGyroCalibrate:
            if (step) gyro::StartCalibration();
            std::snprintf(text, sizeof(text), "%s", gyro::Calibrating() ? "KEEP STILL" : "START");
            break;
    }
}

void StepInputRow(int id, int step, char (&text)[64]) {
    Settings& g = Get();
    if (bool* on = ToggleFlag(id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (float* sensitivity = SensitivityField(id)) {
        // 0.1 to 20.0 in steps of 0.1, shown as five times the stored value so the game's own reads 5.0
        int shown = std::clamp(int(std::lround(*sensitivity * 50.0f)), 1, 200);
        if (step) {
            shown = StepRange(shown, 1, 200, 1, step);
            *sensitivity = float(shown) / 50.0f;
        }
        std::snprintf(text, sizeof(text), "%.1f", float(shown) / 10.0f);
    } else if (id == kDrawDistance) {
        uint32_t v = kDistances[StepTable(kDistances, g.draw_distance, step)];
        std::snprintf(text, sizeof(text), v == 1 ? "DEFAULT" : "%ux", v);
    } else if (id == kShadowResolution) {
        uint32_t size = kShadowSizes[StepTable(kShadowSizes, g.shadow_resolution, step)];
        if (size) std::snprintf(text, sizeof(text), size == 1344 ? "%ux%u (DEFAULT)" : "%ux%u", size, size);
        else std::snprintf(text, sizeof(text), "AUTOMATIC");
    } else if (id == kFov || id == kViewmodelFov) {
        // 45 to 140 in steps of 1
        float& setting = id == kFov ? g.fov : g.viewmodel_fov;
        int fov = StepRange(int(std::lround(setting)), 45, 140, 1, step);
        if (step) setting = float(fov);
        std::snprintf(text, sizeof(text), fov == 90 ? "%d (DEFAULT)" : "%d", fov);
    }
}

void StepRow(int id, int step, app::Settings& s, char (&text)[64]) {
    switch (id) {
        case kMonitor:
            StepMonitor(step, s, text);
            break;
        case kDisplayMode: {
            static const char* const kModes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
            int count = int(std::size(kModes));
            if (step) s.display_mode = app::DisplayMode(StepIndex(int(s.display_mode), step, count));
            std::snprintf(text, sizeof(text), "%s", kModes[std::min(int(s.display_mode), count - 1)]);
            break;
        }
        case kResolution: {
            app::Mode m = StepResolution(s, step);
            std::snprintf(text, sizeof(text), "%ux%u", m.width, m.height);
            break;
        }
        case kRenderResolution:
            StepRenderResolution(s, step, text);
            break;
        case kVsync:
            Flip(s.vsync, step);
            std::snprintf(text, sizeof(text), "%s", OnOff(s.vsync));
            break;
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == app::kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            break;
        }
        case kAntiAliasing:
            StepAntiAliasing(s, step, text);
            break;
        case kFrameGeneration:
            StepFrameGeneration(step, text);
            break;
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, s.audio_output, step)]);
            break;
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            break;
        case kColourBlind:
            StepName(Get().colour_blind, kColourBlindNames, step, text);
            break;
        case kShadowFilter:
            StepName(Get().shadow_filter, kShadowFilterNames, step, text);
            break;
        case kAmbientOcclusion:
            StepName(Get().ambient_occlusion, kAmbientOcclusionNames, step, text);
            if (Get().ambient_occlusion == gpu::kAoRtao && !gpu::RayTracing()) {
                std::snprintf(text, sizeof(text), "RTAO (NOT SUPPORTED)");
            }
            break;
        case kTextureReplacement:
            Flip(s.custom_textures, step);
            std::snprintf(text, sizeof(text), "%s", OnOff(s.custom_textures));
            break;
        case kTextureDumping:
            Flip(s.dump_textures, step);
            std::snprintf(text, sizeof(text), "%s", OnOff(s.dump_textures));
            break;
        case kDumpFormat:
            if (step) s.texture_dump_format = uint32_t(StepIndex(int(s.texture_dump_format), step, 2));
            std::snprintf(text, sizeof(text), "%s", kDumpFormatNames[std::min(s.texture_dump_format, 1u)]);
            break;
        case kGyroSensitivity:
        case kGyroAxis:
        case kGyroButton:
        case kGyroButtonMode:
        case kGyroCalibrate:
            StepGyroRow(id, step, text);
            break;
        default:
            StepInputRow(id, step, text);
            break;
    }
}

void Changed(bool display) {
    g_revision++;
    if (display) app::ApplyDisplay();
    audio::SetOutputMode(app::Get().audio_output);
    app::Save();
}

// on a copy of the settings a step is only shown, not saved
std::string RowText(int id, int step, app::Settings& s = app::Get()) {
    char text[64] = "";
    StepRow(id, step, s, text);
    if (step && &s == &app::Get()) Changed(DisplayRow(id));
    return text;
}

// the MOUSE AND KEYBOARD page's settings
void CopyMouseSettings(Settings& to, const Settings& from) {
    to.sensitivity_x = from.sensitivity_x;
    to.sensitivity_y = from.sensitivity_y;
    to.aim_sensitivity_x = from.aim_sensitivity_x;
    to.aim_sensitivity_y = from.aim_sensitivity_y;
    to.invert_x = from.invert_x;
    to.invert_y = from.invert_y;
    to.hold_aim = from.hold_aim;
    to.hold_sprint = from.hold_sprint;
}

void RestoreDefaults(int page) {
    if (page == kDebugPage) {
        Get().invincible = false;
        Get().noclip = false;
        Changed(false);
        return;
    }
    if (page == kKeyboardPage) {
        CopyMouseSettings(Get(), Settings{});
        keys::ResetBinds();
        Changed(false);
        return;
    }
    const app::Settings def;
    app::Settings& s = app::Get();
    s.display_mode = def.display_mode;
    s.width = def.width;
    s.height = def.height;
    s.render_percent = def.render_percent;
    s.vsync = def.vsync;
    s.fps_cap = def.fps_cap;
    s.aniso = def.aniso;
    s.msaa = def.msaa;
    s.audio_output = def.audio_output;
    s.custom_textures = def.custom_textures;
    s.dump_textures = def.dump_textures;
    s.texture_dump_format = def.texture_dump_format;
    // the mouse settings belong to the other page
    const Settings keep = Get();
    Get() = Settings{};
    CopyMouseSettings(Get(), keep);
    Get().invincible = keep.invincible;
    Get().noclip = keep.noclip;
    Changed(true);
}

std::string BindText(int action) {
    std::string text = keys::KeyLabel(keys::Key(action, 0));
    if (uint8_t alt = keys::Key(action, 1)) text += (text.empty() ? "" : "/") + keys::KeyLabel(alt);
    return text.empty() ? "UNBOUND" : text;
}

// Panel

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// label left, arrows and value right, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize();
    float row_w = ImGui::GetContentRegionAvail().x;
    float row_h = ImGui::GetFrameHeight();
    float value_w = em * kValueEms;
    float block_w = row_h * 2 + value_w + style.ItemSpacing.x * 2;
    float row_x = ImGui::GetCursorPosX();
    float row_y = ImGui::GetCursorPosY();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row_w, row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    ImGui::SetCursorPos(ImVec2(row_x + style.ItemInnerSpacing.x, row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = row_x + row_w - block_w;
    ImGui::SetCursorPos(ImVec2(block_x, row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + row_h + style.ItemSpacing.x;
    float pad = (value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + value_w + style.ItemSpacing.x, row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    // an item, not a cursor move, so the child window grows to the row
    ImGui::SetCursorPos(ImVec2(row_x, row_y));
    ImGui::Dummy(ImVec2(row_w, row_h));
    return step;
}

const char* const kPages[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD", "DEBUG"};

// the debug page is default.xex's only
int PageCount() { return g_load_level ? int(std::size(kPages)) : kDebugPage; }

// the bind row waiting for a key, -1 when idle, and its box: 0 the key, 1 the alternate
int g_capture = -1;
int g_capture_slot = 0;

constexpr float kKeyBoxEms = 10.0f;  // a key box's width, two to a row

// label left, a box per key right, ending where the other rows' values do; Enter or a click on a box starts its
// capture, a right click or Delete on it clears it
void BindRow(const Row& r) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize();
    float row_w = ImGui::GetContentRegionAvail().x;
    float row_h = ImGui::GetFrameHeight();
    float row_x = ImGui::GetCursorPosX();
    float row_y = ImGui::GetCursorPosY();
    float value_end = row_x + row_w - row_h - style.ItemSpacing.x;
    float box_w = em * kKeyBoxEms;
    ImGui::PushID(r.label);
    ImGui::SetCursorPos(ImVec2(row_x + style.ItemInnerSpacing.x, row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(r.label);
    for (int slot = 0; slot < 2; slot++) {
        float x = value_end - float(2 - slot) * box_w - float(1 - slot) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row_y));
        ImGui::PushID(slot);
        bool capturing = g_capture == r.id && g_capture_slot == slot;
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row_h)) && g_capture < 0) {
            g_capture = r.id;
            g_capture_slot = slot;
            keys::BeginCapture();
        }
        bool clear = ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                     (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false));
        if (clear && g_capture < 0) {
            keys::Bind(r.id, 0, slot);
            Changed(false);
        }
        bool lit = capturing || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        std::string value = capturing ? "PRESS A KEY" : keys::KeyLabel(keys::Key(r.id, slot));
        if (!capturing && slot == 0 && value.empty()) value = "UNBOUND";
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::SetCursorPos(ImVec2(row_x, row_y));
    ImGui::Dummy(ImVec2(row_w, row_h));
}

// label left, the row is the nav stop; Enter or a click loads the level and closes the panel, before START at the
// title screen nothing happens
void LevelRow(const Row& r) {
    float row_w = ImGui::GetContentRegionAvail().x;
    float row_h = ImGui::GetFrameHeight();
    float row_x = ImGui::GetCursorPosX();
    float row_y = ImGui::GetCursorPosY();
    ImGui::PushID(r.label);
    bool load = ImGui::Selectable("##row", false, 0, ImVec2(row_w, row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
    }
    ImGui::PopID();
    ImGui::SetCursorPos(ImVec2(row_x + ImGui::GetStyle().ItemInnerSpacing.x, row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(r.label);
    ImGui::SetCursorPos(ImVec2(row_x, row_y));
    ImGui::Dummy(ImVec2(row_w, row_h));
    if (load && g_load_level(kLevels[r.id].folder)) ToggleMenu();
}

// a key or mouse button binds, Escape cancels
void PollCapture() {
    if (g_capture < 0) return;
    int vk = keys::Captured();
    if (!vk) return;
    if (vk > 0) {
        keys::Bind(g_capture, uint8_t(vk), g_capture_slot);
        Changed(false);
    }
    g_capture = -1;
}
bool g_confirm = false;  // Defaults asks first
std::atomic<int> g_page{kSettingsPage};
std::atomic<bool> g_page_set{true};  // the tab bar picks g_page on its next frame

// on F10 the monitor, display mode and resolution wait for Apply, as Windows' display settings do; closing drops them
bool g_display_held = false;
app::Settings g_display;

void HoldDisplay(int id, int step) {
    const app::Settings& s = app::Get();
    if (!g_display_held) {
        g_display_monitor = Get().monitor;
        g_display.display_mode = s.display_mode;
        g_display.width = s.width;
        g_display.height = s.height;
    }
    RowText(id, step, g_display);
    g_display_held = g_display_monitor != Get().monitor || g_display.display_mode != s.display_mode ||
                     g_display.width != s.width || g_display.height != s.height;
}

void Apply() {
    app::Settings& s = app::Get();
    Get().monitor = g_display_monitor;
    s.display_mode = g_display.display_mode;
    s.width = g_display.width;
    s.height = g_display.height;
    g_display_held = false;
    Changed(true);
}

void DrawRows(int page) {
    for (const Row& r : rows::Page(page)) {
        if (r.kind == Kind::Heading) {
            Section(r.label);
            continue;
        }
        if (r.kind == Kind::Bind) BindRow(r);
        if (r.kind == Kind::Level) LevelRow(r);
        if (r.kind != Kind::Value) continue;
        bool held = g_display_held && DisplayRow(r.id);
        int step = OptionRow(r.label, RowText(r.id, 0, held ? g_display : app::Get()).c_str());
        if (step && DisplayRow(r.id)) HoldDisplay(r.id, step);
        else if (step) RowText(r.id, step);
    }
}

// one tab a page, as wide as they share the line; LB / RB step them as the game's menus do
void DrawTabs() {
    int count = PageCount();
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    if (step && !g_confirm) {
        g_page = StepIndex(g_page, step, count);
        g_page_set = true;
    }
    // a page set here or from the game's menus shows from the next frame, the tab drawn this one still reads as open
    bool set = g_page_set.exchange(false);
    ImGui::PushStyleColor(ImGuiCol_Tab, kTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kTabHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##pages")) {
        float gaps = ImGui::GetStyle().ItemInnerSpacing.x * float(count - 1);
        float w = (ImGui::GetContentRegionAvail().x - gaps) / float(count);
        for (int i = 0; i < count; i++) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginTabItem(kPages[i], nullptr, set && i == g_page ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!set) g_page = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void ConfirmDefaults() {
    if (!g_confirm) return;
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove;
    ImGui::OpenPopup("##defaults");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##defaults", nullptr, kFlags)) return;
    ImGui::TextUnformatted("Restore every setting on this page to its default?");
    float bw = ImGui::GetFontSize() * 8;
    if (ImGui::Button("Yes", ImVec2(bw, 0))) {
        g_display_held = false;
        RestoreDefaults(g_page);
        g_confirm = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("No", ImVec2(bw, 0))) g_confirm = false;
    if (!g_confirm) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 50, em * 32), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        const char* title = "SETTINGS";
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::TextUnformatted(title);
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        DrawRows(g_page);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        const char* hint = "Up and Down select, Left and Right change, Escape closes";
        if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
        else if (g_page == kKeyboardPage) hint = "Enter or a click rebinds, right click or Delete clears the key";
        else if (g_page == kDebugPage) hint = "Enter or a click loads the level, from the main menu or in game";
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        float bw = em * 8;
        float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
        if (ImGui::Button("Back", ImVec2(bw, 0))) ToggleMenu();
        ImGui::SameLine();
        if (ImGui::Button("Defaults", ImVec2(bw, 0)) && g_capture < 0) g_confirm = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_display_held);
        if (ImGui::Button("Apply", ImVec2(bw, 0))) Apply();
        ImGui::EndDisabled();
        ConfirmDefaults();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

uint32_t SettingsRevision() { return g_revision; }

void ToggleMenu() {
    std::lock_guard lock(app::UiLock());
    g_open = !g_open;
    g_display_held = false;
    g_confirm = false;
    g_capture = -1;
}

void OpenMenuPage(int page) {
    std::lock_guard lock(app::UiLock());
    g_page = std::clamp(page, 0, PageCount() - 1);
    g_page_set = true;
    g_display_held = false;
    g_confirm = false;
    g_open = true;
}

void DrawUi(double) {
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    // the key being captured must not also move the selection
    if (g_capture >= 0) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!MenuOpen()) return;
    PollCapture();
    DrawPanel();
}

// the settings panel key opens and closes the panel, Escape closes it; a capture takes every key
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    // onto the MONITOR setting's monitor first, the host then sizes the window on the monitor it is on
    if (msg == WM_CREATE) {
        UpdateSplashMonitor();
        UpdateWindowMonitor(hwnd);
        return false;
    }
    if (msg == kHostApplyDisplay) {
        UpdateWindowMonitor(hwnd);
        return false;
    }
    bool key_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    bool capturing = keys::Capturing();
    uint8_t key = keys::PositionKey(wp);
    bool panel_key = key && (key == keys::Key(keys::kSettings, 0) || key == keys::Key(keys::kSettings, 1));
    if (key_down && !capturing && (panel_key || (wp == VK_ESCAPE && MenuOpen()))) {
        ToggleMenu();
        *result = 0;
        return true;
    }
    if (CursorMessage(hwnd, msg, lp, result)) return true;
    MenuMouseMessage(hwnd, msg, wp, lp);
    bool taken = keys::Message(hwnd, msg, wp, lp) || (capturing && key_down);
    if (taken) *result = 0;
    return taken;
}

namespace rows {

std::span<const Row> Page(int page) {
    if (page == kSettingsPage) return kSettingsRows;
    if (page == kKeyboardPage) return KeyboardRows();
    if (page == kDebugPage) return DebugRows();
    return {};
}

std::string Text(const Row& row, int step) {
    std::lock_guard lock(app::UiLock());
    if (row.kind == Kind::Bind) return BindText(row.id);
    if (row.kind != Kind::Value) return {};
    return RowText(row.id, step);
}

void SetBind(int action, uint8_t vk) {
    std::lock_guard lock(app::UiLock());
    keys::Bind(action, vk, 0);
    Changed(false);
}

void ClearBind(int action) {
    std::lock_guard lock(app::UiLock());
    keys::Bind(action, 0, 0);
    keys::Bind(action, 0, 1);
    Changed(false);
}

std::vector<std::string> Choices(const Row& row, int* current) {
    std::lock_guard lock(app::UiLock());
    std::vector<std::string> out;
    *current = 0;
    // calibrating has no values, a step starts it
    if (row.kind != Kind::Value || row.id == kGyroCalibrate) return out;
    const Settings keep = Get();
    const std::string monitor = g_display_monitor;
    g_picks = 0;
    for (g_pick = 0; g_pick < std::max(g_picks, 1); g_pick++) {
        g_display_monitor = Get().monitor;  // a copy of the settings reads its monitor from here
        app::Settings copy = app::Get();
        char text[64] = "";
        StepRow(row.id, 1, copy, text);
        Get() = keep;
        out.push_back(text);
    }
    g_pick = -1;
    g_display_monitor = monitor;
    *current = g_picked;
    if (g_picks < 2) out.clear();
    return out;
}

void Choose(const Row& row, int index) {
    std::lock_guard lock(app::UiLock());
    g_pick = index;
    RowText(row.id, 1);
    g_pick = -1;
}

void Defaults(int page) {
    std::lock_guard lock(app::UiLock());
    RestoreDefaults(page);
}

}  // namespace rows

}  // namespace fcip
