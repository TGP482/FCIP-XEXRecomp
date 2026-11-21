// custom levels: a folder in the custom levels folder is one when its LevelData.xml names the level it was made from
// (LevelInfo CampaignLevel), a campaign level or an editor template, as the level editor saves them; a copy of its
// mission's string table there replaces the campaign's
#include "common/custom_levels.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string_view>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kCampaignLevelKey = "CampaignLevel=\"";
constexpr const char* kLanguages = "LANGUAGES/";  // the string tables' folder as Language loads them
constexpr bool kEditor = std::string_view(IMAGE_XEX) == "ige.xex";
constexpr const char* kCustomFolder = kEditor ? "saved" : "play";
// a level pack's FAT: "DAT2", the count, then offset, stored size, size and CRC each, by CRC; stored equal to size is
// raw data
constexpr uint32_t kPackMagic = 0x32544144;
constexpr size_t kFatHeaderBytes = 8;
constexpr size_t kFatEntryBytes = 16;
constexpr const char* kEditorTemplatesPack = "MET_Dependency";  // the editor templates' shared assets
constexpr const char* kEditorTemplatePrefix = "met_";
constexpr uint32_t kPolynomial = 0xEDB88320;  // zlib's CRC32, which the packs key their files by (823CB118)
// a dependency list's files: <File CRC="n", n the CRC of the file's path, the key its pack holds it by
constexpr const char* kDependencyLists[] = {"_DepLoad.xml", "_DepLoadParticles.xml"};
constexpr const char* kFileCrcKey = "<File CRC=\"";

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

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (i * 8)));
}

uint32_t GetLe32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct PackedFile {
    uint32_t crc;
    fs::path path;
};

struct PackEntry {
    uint32_t offset;
    uint32_t stored;
    uint32_t size;
    uint32_t crc;
};

// a base level's pack: its data file and the entries the custom level takes from it
struct BasePack {
    std::string dat;
    std::vector<PackEntry> entries;
};

std::vector<PackEntry> FatEntries(const std::vector<uint8_t>& fat) {
    if (fat.size() < kFatHeaderBytes || GetLe32(fat.data()) != kPackMagic) return {};
    size_t count = std::min<size_t>(GetLe32(fat.data() + 4), (fat.size() - kFatHeaderBytes) / kFatEntryBytes);
    std::vector<PackEntry> entries;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* e = fat.data() + kFatHeaderBytes + i * kFatEntryBytes;
        entries.push_back({GetLe32(e), GetLe32(e + 4), GetLe32(e + 8), GetLe32(e + 12)});
    }
    return entries;
}

// the entry count a pack's FAT holds, none when it has no FAT
size_t FatCount(const fs::path& fat) {
    uint8_t header[kFatHeaderBytes] = {};
    std::ifstream in(fat, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(header), kFatHeaderBytes)) return 0;
    return GetLe32(header + 4);
}

// the folder's files, keyed as the game names them, \levels\<level>\<file> in lower case; stale is set when one is
// newer than the pack
std::vector<PackedFile> PackedFiles(const std::string& level, const fs::path& fat, const fs::path& dat, bool& stale) {
    fs::path folder = fcip::CustomLevelFolder(level);
    std::error_code ec;
    fs::file_time_type packed = fs::last_write_time(fat, ec);
    stale = bool(ec);
    std::vector<PackedFile> files;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec) || e.path() == fat || e.path() == dat) continue;
        std::string name = fs::relative(e.path(), folder, ec).string();
        std::replace(name.begin(), name.end(), '/', '\\');
        files.push_back({fcip::Crc32(Lower("\\levels\\" + level + "\\" + name)), e.path()});
        if (!stale && e.last_write_time(ec) > packed) stale = true;
    }
    return files;
}

// the files the custom level's dependency lists name, by CRC
std::unordered_set<uint32_t> DependencyFiles(const fs::path& folder, const std::string& level) {
    std::unordered_set<uint32_t> crcs;
    size_t key = std::char_traits<char>::length(kFileCrcKey);
    for (const char* suffix : kDependencyLists) {
        std::ifstream in(folder / (level + suffix), std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(in), {});
        for (size_t at = text.find(kFileCrcKey); at != std::string::npos; at = text.find(kFileCrcKey, at + key)) {
            crcs.insert(uint32_t(std::strtoul(text.c_str() + at + key, nullptr, 10)));
        }
    }
    return crcs;
}

// a level's pack entries the custom level does not have yet, every one or only those wanted
bool TakeFrom(const std::string& level, const std::unordered_set<uint32_t>* wanted, std::unordered_set<uint32_t>& taken,
              std::vector<BasePack>& packs) {
    std::string path = "game:\\Levels\\" + level + "\\" + level;
    std::vector<uint8_t> fat;
    if (!kernel::ReadGameFile(path + ".fat", fat)) return false;
    BasePack pack{path + ".dat", {}};
    for (const PackEntry& e : FatEntries(fat)) {
        if (wanted && !wanted->count(e.crc)) continue;
        if (taken.insert(e.crc).second) pack.entries.push_back(e);
    }
    if (!pack.entries.empty()) packs.push_back(std::move(pack));
    return true;
}

// FCIP.exe streams a level's files only from the level's own pack (823EDCC8 finds no read handle for another pack),
// so a custom level's pack also holds what its base level's pack has, and for a level made from an editor template
// the templates' shared pack, each file it does not have itself; then the files its dependency lists name from the
// campaign level holding them, for objects the editor placed from other levels
std::vector<BasePack> BasePacks(const std::string& level, const std::string& base,
                                const std::vector<PackedFile>& files) {
    if (kEditor || base.empty()) return {};
    std::vector<std::string> levels = {base};
    if (Lower(base).starts_with(kEditorTemplatePrefix)) levels.push_back(kEditorTemplatesPack);
    std::unordered_set<uint32_t> taken;
    for (const PackedFile& f : files) taken.insert(f.crc);
    std::vector<BasePack> packs;
    for (const std::string& from : levels) {
        if (!TakeFrom(from, nullptr, taken, packs)) LOGE("custom levels: %s.fat could not be read", from.c_str());
    }
    std::unordered_set<uint32_t> wanted = DependencyFiles(fcip::CustomLevelFolder(level), level);
    for (const char* campaign : fcip::kCampaignLevels) {
        if (Lower(campaign) != Lower(base)) TakeFrom(campaign, &wanted, taken, packs);
    }
    return packs;
}

}  // namespace

namespace fcip {

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

void EnsureLevelPack(const std::string& level) {
    fs::path folder = CustomLevelFolder(level);
    fs::path fat = folder / (level + ".fat");
    fs::path dat = folder / (level + ".dat");
    bool stale = false;
    std::vector<PackedFile> files = PackedFiles(level, fat, dat, stale);
    std::vector<BasePack> bases = BasePacks(level, CampaignOf(folder / "LevelData.xml"), files);
    size_t count = files.size();
    for (const BasePack& b : bases) count += b.entries.size();
    if (!stale) stale = FatCount(fat) != count;
    if (!stale || files.empty()) return;
    std::vector<PackEntry> entries;
    std::ofstream out(dat, std::ios::binary);
    uint32_t offset = 0;
    for (const PackedFile& f : files) {
        std::ifstream in(f.path, std::ios::binary);
        std::string data(std::istreambuf_iterator<char>(in), {});
        if (Lower(f.path.extension().string()) == ".xml") data = DropArrayElements(data);  // levels saved before
        out.write(data.data(), std::streamsize(data.size()));
        uint32_t size = uint32_t(data.size());
        entries.push_back({offset, size, size, f.crc});
        offset += size;
    }
    for (const BasePack& b : bases) {
        std::vector<uint8_t> data;
        if (!kernel::ReadGameFile(b.dat, data)) LOGE("custom levels: %s could not be read", b.dat.c_str());
        for (const PackEntry& e : b.entries) {
            if (uint64_t(e.offset) + e.stored > data.size()) continue;
            out.write(reinterpret_cast<const char*>(data.data() + e.offset), std::streamsize(e.stored));
            entries.push_back({offset, e.stored, e.size, e.crc});
            offset += e.stored;
        }
    }
    out.close();
    std::sort(entries.begin(), entries.end(), [](const PackEntry& a, const PackEntry& b) { return a.crc < b.crc; });
    std::vector<uint8_t> table;
    PutLe32(table, kPackMagic);
    PutLe32(table, uint32_t(entries.size()));
    for (const PackEntry& e : entries) {
        for (uint32_t v : {e.offset, e.stored, e.size, e.crc}) PutLe32(table, v);
    }
    const char* bytes = reinterpret_cast<const char*>(table.data());
    std::ofstream(fat, std::ios::binary).write(bytes, std::streamsize(table.size()));
    LOGR("custom levels: %s packed, %zu files, %u bytes", level.c_str(), entries.size(), offset);
}

std::string DropArrayElements(const std::string& xml) {
    static const std::regex kArray(R"([ \t]*<[A-Za-z_]\w*(?: [0-9]+="[^"]*")+ */>\r?\n?)");
    return std::regex_replace(xml, kArray, "");
}

fs::path CustomLevelsFolder() { return app::ExeDir() / "levels" / kCustomFolder; }

fs::path NewLevelsFolder() { return app::ExeDir() / "levels" / "new"; }

fs::path CustomLevelFolder(const std::string& level) {
    bool unsaved = Lower(level).starts_with(Lower(kUnsavedLevelPrefix));
    return (unsaved ? NewLevelsFolder() : CustomLevelsFolder()) / level;
}

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
    fs::path folder = CustomLevelFolder(path.substr(kLevelsLength, end - kLevelsLength));
    return fs::is_directory(folder, ec) ? folder / path.substr(end + 1) : fs::path();
}

fs::path OwnStringTable(const std::string& level, const std::string& file) {
    std::error_code ec;
    fs::path path = CustomLevelFolder(level) / file;
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
