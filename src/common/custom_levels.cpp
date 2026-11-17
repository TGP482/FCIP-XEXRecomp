// custom levels: a folder in levels\ beside the exe is one when its LevelData.xml names the campaign level it was made
// from (LevelInfo CampaignLevel), as the level editor saves them; a copy of its mission's string table there replaces
// the campaign's
#include "common/custom_levels.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "app/app.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kCampaignLevelKey = "CampaignLevel=\"";
constexpr const char* kLanguages = "LANGUAGES/";  // the string tables' folder as Language loads them

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

fs::path OwnStringTable(const std::string& level, const std::string& file) {
    std::error_code ec;
    fs::path path = app::ExeDir() / "levels" / level / file;
    return !level.empty() && !file.empty() && fs::is_regular_file(path, ec) ? path : fs::path();
}

std::string OwnStringTableName(const std::string& level, const std::string& file) { return level + "/" + file; }

std::string OwnStringTablePath(const std::string& level, const std::string& path) {
    if (!path.starts_with(kLanguages) || level.empty()) return {};
    size_t at = path.find('/', std::char_traits<char>::length(kLanguages));  // after the language
    std::string own = level + "/";
    if (at == std::string::npos || path.compare(at + 1, own.size(), own) != 0) return {};
    return "\\Levels\\" + level + "\\" + path.substr(at + 1 + own.size());
}

}  // namespace fcip
