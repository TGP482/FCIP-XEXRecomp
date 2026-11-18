// the solo level editor's save: objective markers and the extraction, added to the saved mission (level_rules.cpp)
#pragma once
#include <string>

namespace fcip::editor {

struct ObjectiveRules {
    std::string mission;  // with the markers and the extraction's end trigger
    std::string script;   // the rules file (fcip::kRulesFile), empty without an extraction
};

// every objective waypoint without a marker gets one; extraction names the waypoint the level ends at, empty for none;
// next_map is the end trigger's NextMap, which a custom level's end takes to the next custom level
ObjectiveRules AddObjectiveRules(const std::string& mission, const std::string& extraction,
                                 const std::string& next_map);

}  // namespace fcip::editor
