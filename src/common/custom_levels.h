// custom levels: folders in the custom levels folder, laid over game:\Levels, each made from a campaign level or an
// editor template; the level editor saves them in levels\saved beside the exe and FCIP.exe plays the ones in
// levels\play, as the texture dumper and the texture replacement keep their own folders
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fcip {

// both campaigns, by their folder under assets\levels
inline constexpr const char* kCampaignLevels[] = {
    "00_TRAINING", "01_BEACH", "02_NATIVE", "03_RIVER", "04_RAIN_FOREST", "05_MINES", "06_CLIFFS",
    "07_SUB-AQUATIC_FACILITY", "10_DEAD_MARSHES", "11_WASTELAND", "13_BLACK_FOREST", "21_DUNEA", "22_DUNEB",
    "23_DUNEC", "24_SHANTYDAY", "26_SHANTYNIGHT", "28_JUNGLE", "29_FOREST", "30_MOUNTAIN",
};

// zlib's CRC32, which the packs key their files by (823CB118) and dependency lists their entries
uint32_t Crc32(const std::string& text);

struct CustomLevel {
    std::string name;      // its folder, the level name the game starts
    std::string base;      // the level its assets come from, a campaign level or an editor template
};

std::filesystem::path CustomLevelsFolder();  // this exe's: levels\saved or levels\play
// the level editor's new levels until Save As names them, levels\new, emptied as the editor starts
std::filesystem::path NewLevelsFolder();
inline constexpr const char* kUnsavedLevelPrefix = "UNSAVED_";  // a new level's name there
std::filesystem::path CustomLevelFolder(const std::string& level);  // in either folder
std::vector<CustomLevel> FindCustomLevels();  // any thread, in folder name order
// the game streams a level's sectors by the CRC of their path, which only a pack's FAT answers, so a custom level's
// folder is packed into <level>.dat and <level>.fat, the pack the level start opens, in FCIP.exe with its base level's
// files; made again when a file is newer
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
