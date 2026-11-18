// a brand new level's first files, made from an editor template (new_level.cpp): the campaign's 16x16 sectors with the
// template's own at their spot and its seabed around them, and a solo mission
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fcip::editor {

struct LevelFile {
    std::string name;  // inside the level's folder
    std::vector<uint8_t> data;
};

// a game file by its path, levels\<level>\<file>; false when there is none
using ReadGameFile = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

// empty when the template's files are missing
std::vector<LevelFile> NewLevelFiles(const std::string& name, const std::string& base, const ReadGameFile& read);

}  // namespace fcip::editor
