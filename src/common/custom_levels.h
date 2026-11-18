// custom levels: folders in the custom levels folder, laid over game:\Levels, each made from a campaign level or an
// editor template; the level editor saves them in levels\saved beside the exe and FCIP.exe plays the ones in
// levels\play, as the texture dumper and the texture replacement keep their own folders
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fcip {

struct CustomLevel {
    std::string name;      // its folder, the level name the game starts
    std::string base;      // the level its assets come from, a campaign level or an editor template
};

std::filesystem::path CustomLevelsFolder();  // this exe's: levels\saved or levels\play
// a new level's working folder in levels\saved until Save As names it, never listed
inline constexpr const char* kUnsavedLevelPrefix = "UNSAVED_";
std::vector<CustomLevel> FindCustomLevels();  // any thread, in folder name order
// the game streams a level's sectors by the CRC of their path, which only a pack's FAT answers, so a custom level's
// folder is packed into <level>.dat and <level>.fat, the pack the level start opens; made again when a file is newer
void EnsureLevelPack(const std::string& level);
bool EvolutionLevel(const std::string& level);  // Evolution's levels are 2x and 3x
// the path names the level's own Mission.lua, Levels\<level>\Mission.lua in any case: the mission script of a level
// made from an editor template
bool IsOwnScript(const std::string& level, const std::string& path);
// the host file a game path (game:\Levels\<level>\..., \Levels\... or Levels/...) names in a custom level's folder,
// empty when it names none
std::filesystem::path CustomLevelFile(std::string path);

// a custom level's own copy of its mission's string table, <level>\<file> in its folder: the file, empty when it has
// none; the name it loads by, its own so the campaign's table the game keeps loaded is not taken for it; and the game
// path a load of that name opens instead of LANGUAGES/<language>/<name>, empty for any other path; a table named after
// the level, as one made from an editor template has, opens its copy under its plain name too
std::filesystem::path OwnStringTable(const std::string& level, const std::string& file);
std::string OwnStringTableName(const std::string& level, const std::string& file);
std::string OwnStringTablePath(const std::string& level, const std::string& path);

}  // namespace fcip
