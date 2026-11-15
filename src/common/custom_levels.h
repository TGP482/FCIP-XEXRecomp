// custom levels: folders in levels\ beside the exe, laid over game:\Levels, each made from a campaign level
#pragma once
#include <string>
#include <vector>

namespace fcip {

struct CustomLevel {
    std::string name;      // its folder, the level name the game starts
    std::string campaign;  // the campaign level its assets come from
};

std::vector<CustomLevel> FindCustomLevels();  // any thread, in folder name order

}  // namespace fcip
