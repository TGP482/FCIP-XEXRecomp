// the campaign's levels, which the solo editor opens as the game loads them (campaign_levels.cpp)
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/custom_levels.h"

struct PPCContext;

namespace fcip::editor {

using fcip::kCampaignLevels;

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
// the far vegetation sprites a level's vegbank.dat names, as level files (new_level.cpp)
std::vector<std::string> SpriteFiles(const std::vector<uint8_t>& vegbank);
// its dependency lists, <name>_DepLoad.xml and <name>_DepLoadParticles.xml, into folder
void WriteDependencyLists(PPCContext& ctx, const std::string& base, const std::filesystem::path& folder,
                          const std::string& name);

// the LEVEL MANAGER (level_manager.cpp): OPEN A LEVEL's list over the page, window thread
bool LevelListUp();
void CloseLevelList();
void LevelManagerShown();                                 // editor thread, its handler is active
void UpdateLevelManager(PPCContext& ctx, uint32_t page);  // editor thread, as P_MapManager draws

}  // namespace fcip::editor
