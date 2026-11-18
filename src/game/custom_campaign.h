// default.xex: CUSTOM CAMPAIGN, the custom levels in levels\play beside the exe played in folder name order
#pragma once
#include <string>

namespace fcip {

// the next level the menus start is the first custom level; its campaign level, empty when there are none
std::string BeginCustomCampaign();
void CancelCustomCampaign();
bool CustomCampaignPending();  // started from CUSTOM CAMPAIGN, no level started yet
// a queued command as custom levels need it: the text to queue instead, empty to keep it; ours for the debug page's
// own level changes, which go where they say
std::string RouteLevelCommand(const std::string& command, bool ours);

}  // namespace fcip
