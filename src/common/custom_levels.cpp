// custom levels: a folder in the custom levels folder is one when its LevelData.xml names the level it was made from
// (LevelInfo CampaignLevel), a campaign level or an editor template, as the level editor saves them; a copy of its
// mission's string table there replaces the campaign's
#include "common/custom_levels.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kCampaignLevelKey = "CampaignLevel=\"";
constexpr const char* kLanguages = "LANGUAGES/";  // the string tables' folder as Language loads them
constexpr bool kEditor = std::string_view(IMAGE_XEX) == "ige.xex";
constexpr const char* kCustomFolder = kEditor ? "saved" : "play";
// a level pack's FAT: "DAT2", the count, then offset, stored size, size and CRC each, by CRC; stored equal to size is
// raw data
constexpr uint32_t kPackMagic = 0x32544144;
constexpr uint32_t kPolynomial = 0xEDB88320;  // zlib's CRC32, which the packs key their files by (823CB118)

std::string CampaignOf(const fs::path& level_data) {
    std::ifstream in(level_data, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(in), {});
    size_t at = text.find(kCampaignLevelKey);
    if (at == std::string::npos) return {};
    at += std::char_traits<char>::length(kCampaignLevelKey);
    size_t end = text.find('"', at);
    return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

uint32_t Crc32(const std::string& text) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? kPolynomial ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : text) crc = table[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (i * 8)));
}

struct PackedFile {
    uint32_t crc;
    fs::path path;
};

// the folder's files, keyed as the game names them, \levels\<level>\<file> in lower case; stale is set when one is
// newer than the pack
std::vector<PackedFile> PackedFiles(const std::string& level, const fs::path& fat, const fs::path& dat, bool& stale) {
    fs::path folder = fcip::CustomLevelsFolder() / level;
    std::error_code ec;
    fs::file_time_type packed = fs::last_write_time(fat, ec);
    stale = bool(ec);
    std::vector<PackedFile> files;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec) || e.path() == fat || e.path() == dat) continue;
        std::string name = fs::relative(e.path(), folder, ec).string();
        std::replace(name.begin(), name.end(), '/', '\\');
        files.push_back({Crc32(Lower("\\levels\\" + level + "\\" + name)), e.path()});
        if (!stale && e.last_write_time(ec) > packed) stale = true;
    }
    std::sort(files.begin(), files.end(), [](const PackedFile& a, const PackedFile& b) { return a.crc < b.crc; });
    return files;
}

}  // namespace

namespace fcip {

void EnsureLevelPack(const std::string& level) {
    fs::path folder = CustomLevelsFolder() / level;
    fs::path fat = folder / (level + ".fat");
    fs::path dat = folder / (level + ".dat");
    bool stale = false;
    std::vector<PackedFile> files = PackedFiles(level, fat, dat, stale);
    if (!stale || files.empty()) return;
    std::vector<uint8_t> table;
    PutLe32(table, kPackMagic);
    PutLe32(table, uint32_t(files.size()));
    std::ofstream out(dat, std::ios::binary);
    uint32_t offset = 0;
    for (const PackedFile& f : files) {
        std::ifstream in(f.path, std::ios::binary);
        std::vector<char> data(std::istreambuf_iterator<char>(in), {});
        out.write(data.data(), std::streamsize(data.size()));
        uint32_t size = uint32_t(data.size());
        for (uint32_t v : {offset, size, size, f.crc}) PutLe32(table, v);
        offset += size;
    }
    out.close();
    const char* bytes = reinterpret_cast<const char*>(table.data());
    std::ofstream(fat, std::ios::binary).write(bytes, std::streamsize(table.size()));
    LOGR("custom levels: %s packed, %zu files, %u bytes", level.c_str(), files.size(), offset);
}

fs::path CustomLevelsFolder() { return app::ExeDir() / "levels" / kCustomFolder; }

std::vector<CustomLevel> FindCustomLevels() {
    std::vector<CustomLevel> levels;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(CustomLevelsFolder(), ec)) {
        if (!e.is_directory(ec)) continue;
        std::string base = CampaignOf(e.path() / "LevelData.xml");
        if (!base.empty()) levels.push_back({e.path().filename().string(), base});
    }
    std::sort(levels.begin(), levels.end(), [](const CustomLevel& a, const CustomLevel& b) { return a.name < b.name; });
    return levels;
}

bool EvolutionLevel(const std::string& level) { return !level.empty() && (level[0] == '2' || level[0] == '3'); }

bool IsOwnScript(const std::string& level, const std::string& path) {
    return !level.empty() && Lower(path).ends_with(Lower("levels\\" + level + "\\Mission.lua"));
}

fs::path CustomLevelFile(std::string path) {
    constexpr size_t kLevelsLength = 7;  // "levels\\"
    std::replace(path.begin(), path.end(), '/', '\\');
    std::string lower = Lower(path);
    for (const char* root : {"game:\\", "\\"}) {
        size_t n = std::char_traits<char>::length(root);
        if (lower.compare(0, n, root) != 0) continue;
        path.erase(0, n);
        lower.erase(0, n);
    }
    size_t end = path.find('\\', kLevelsLength);
    if (!lower.starts_with("levels\\") || end == std::string::npos) return {};
    std::error_code ec;
    fs::path folder = CustomLevelsFolder() / path.substr(kLevelsLength, end - kLevelsLength);
    return fs::is_directory(folder, ec) ? folder / path.substr(end + 1) : fs::path();
}

fs::path OwnStringTable(const std::string& level, const std::string& file) {
    std::error_code ec;
    fs::path path = CustomLevelsFolder() / level / file;
    return !level.empty() && !file.empty() && fs::is_regular_file(path, ec) ? path : fs::path();
}

std::string OwnStringTableName(const std::string& level, const std::string& file) { return level + "/" + file; }

std::string OwnStringTablePath(const std::string& level, const std::string& path) {
    if (!path.starts_with(kLanguages) || level.empty()) return {};
    size_t at = path.find('/', std::char_traits<char>::length(kLanguages));  // after the language
    if (at == std::string::npos) return {};
    std::string file = path.substr(at + 1);
    // a level made from an editor template names its table after itself, which no campaign table shares
    if (file == level + ".xml") return "\\Levels\\" + level + "\\" + file;
    std::string own = level + "/";
    if (file.compare(0, own.size(), own) != 0) return {};
    return "\\Levels\\" + level + "\\" + file.substr(own.size());
}

}  // namespace fcip
