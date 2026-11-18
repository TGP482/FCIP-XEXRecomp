// the campaign's levels, which the solo editor opens as the game loads them (campaign_levels.cpp)
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/custom_levels.h"

struct PPCContext;

namespace fcip::editor {

// both campaigns, by their folder under assets\levels
inline constexpr const char* kCampaignLevels[] = {
    "00_TRAINING", "01_BEACH", "02_NATIVE", "03_RIVER", "04_RAIN_FOREST", "05_MINES", "06_CLIFFS",
    "07_SUB-AQUATIC_FACILITY", "10_DEAD_MARSHES", "11_WASTELAND", "13_BLACK_FOREST", "21_DUNEA", "22_DUNEB",
    "23_DUNEC", "24_SHANTYDAY", "26_SHANTYNIGHT", "28_JUNGLE", "29_FOREST", "30_MOUNTAIN",
};

// window thread; an open map is left first, through the editor's exit
void OpenCampaignLevel(int index);
void OpenCustomLevel(const CustomLevel& level);
void ForgetLevelRequest();  // the exit was cancelled
std::string BaseLevel();    // the campaign level open or the level the custom level open was made from, empty for a map
std::filesystem::path OwnScriptPath();  // a custom level's script, levels\<name>\Mission.lua beside the executable
// editor thread: a game file read through the packs, false when there is none (custom_save.cpp)
bool ReadPackFile(PPCContext& ctx, const std::string& file, std::vector<uint8_t>& out);

// the LEVEL MANAGER (level_manager.cpp): OPEN A LEVEL's list over the page, window thread
bool LevelListUp();
void CloseLevelList();
void LevelManagerShown();                                 // editor thread, its handler is active
void UpdateLevelManager(PPCContext& ctx, uint32_t page);  // editor thread, as P_MapManager draws

}  // namespace fcip::editor
