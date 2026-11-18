// custom levels: folders in levels\ beside the exe, laid over game:\Levels, each made from a campaign level or an
// editor template
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fcip {

struct CustomLevel {
    std::string name;      // its folder, the level name the game starts
    std::string base;      // the level its assets come from, a campaign level or an editor template
};

std::vector<CustomLevel> FindCustomLevels();  // any thread, in folder name order
bool EvolutionLevel(const std::string& level);  // Evolution's levels are 2x and 3x
// the path names the level's own Mission.lua, levels\<level>\Mission.lua in any case: the mission script of a level
// made from an editor template
bool IsOwnScript(const std::string& level, const std::string& path);

// a custom level's own copy of its mission's string table, levels\<level>\<file>: the file, empty when it has none;
// the name it loads by, its own so the campaign's table the game keeps loaded is not taken for it; and the game path a
// load of that name opens instead of LANGUAGES/<language>/<name>, empty for any other path
std::filesystem::path OwnStringTable(const std::string& level, const std::string& file);
std::string OwnStringTableName(const std::string& level, const std::string& file);
std::string OwnStringTablePath(const std::string& level, const std::string& path);

}  // namespace fcip
