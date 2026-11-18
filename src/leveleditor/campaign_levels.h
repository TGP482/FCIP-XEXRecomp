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
std::string StartedLevel();  // editor thread: the level folder the game started, a custom level's or a campaign level's
bool IsCampaignLevel(const std::string& name);  // any case
bool CampaignBase();  // the level open is a campaign level or a custom level made from one, not from an editor template
std::filesystem::path OwnScriptPath();  // a custom level's script, levels\saved\<name>\Mission.lua
// editor thread: a game file read through the packs, false when there is none (custom_save.cpp)
bool ReadPackFile(PPCContext& ctx, const std::string& file, std::vector<uint8_t>& out);
void SaveCustomLevel(PPCContext& ctx, uint32_t manager);  // the level open into its folder
void SaveCustomLevelAs(PPCContext& ctx, const std::string& name);  // the same under another name, first its folder
// Save As, window thread: a new level is unsaved until it names it; why a name cannot be taken, empty when it can; the
// name taken, saved on the editor's next update
bool LevelUnsaved();
std::string SaveAsProblem(const std::string& name);
void SaveLevelAs(const std::string& name);
void AskSaveAs();    // editor thread, a save of an unsaved level: the shell opens Save As
bool SaveAsAsked();  // once per ask
// its dependency lists, <name>_DepLoad.xml and <name>_DepLoadParticles.xml, into folder
void WriteDependencyLists(PPCContext& ctx, const std::string& base, const std::filesystem::path& folder,
                          const std::string& name);

// the LEVEL MANAGER (level_manager.cpp): OPEN A LEVEL's list over the page, window thread
bool LevelListUp();
void CloseLevelList();
void LevelManagerShown();                                 // editor thread, its handler is active
void UpdateLevelManager(PPCContext& ctx, uint32_t page);  // editor thread, as P_MapManager draws

}  // namespace fcip::editor
