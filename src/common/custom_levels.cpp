// custom levels: a folder in levels\ beside the exe is one when its LevelData.xml names the campaign level it was made
// from (LevelInfo CampaignLevel), as the level editor saves them
#include "common/custom_levels.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "app/app.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kCampaignLevelKey = "CampaignLevel=\"";

std::string CampaignOf(const fs::path& level_data) {
    std::ifstream in(level_data, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(in), {});
    size_t at = text.find(kCampaignLevelKey);
    if (at == std::string::npos) return {};
    at += std::char_traits<char>::length(kCampaignLevelKey);
    size_t end = text.find('"', at);
    return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

}  // namespace

namespace fcip {

std::vector<CustomLevel> FindCustomLevels() {
    std::vector<CustomLevel> levels;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(app::ExeDir() / "levels", ec)) {
        if (!e.is_directory(ec)) continue;
        std::string campaign = CampaignOf(e.path() / "LevelData.xml");
        if (!campaign.empty()) levels.push_back({e.path().filename().string(), campaign});
    }
    std::sort(levels.begin(), levels.end(), [](const CustomLevel& a, const CustomLevel& b) { return a.name < b.name; });
    return levels;
}

}  // namespace fcip
