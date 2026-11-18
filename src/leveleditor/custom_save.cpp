// ige.xex, the solo level editor: a campaign level, or a level made from an editor template, saves as a custom level,
// its own folder levels\saved\<name>\ beside the executable, which is laid over game:\Levels. The editor's own save
// (82109BF8) puts a map into a user map, a 4 MB blob with room for 255 files, which a campaign level's 550 files and
// 40 MB do not fit. Its writers make real files when no pack
// takes them (File_Open 8234EF80), so the save runs them with the engine's level path on the custom level: terrain,
// sectors, tools, ziplines and mounted weapons. The rest are written here: the campaign's level files the editor does
// not write, its dependency lists with the editor templates' added, its sequences as edited, its LevelData.xml naming
// the campaign level, its mission's string table when its text was edited, the mission with the editor's
// entities and AI objects, the AI navigation cut for its areas (and its brushes while it has no graphs), the paint
// layers, and a first Mission.lua that runs after the campaign's mission script. Save As copies the folder the level
// saved to under the name typed first; a new level saves only through it.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/mission_text.h"
#include "leveleditor/mode.h"
#include "leveleditor/nav_graph.h"
#include "leveleditor/sequences.h"
#include "leveleditor/static_geometry.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

namespace fs = std::filesystem;

// the campaign level's files the editor's save does not write, copied from its pack
constexpr const char* kCopiedFiles[] = {"Brush.lst", "vegbank.dat", "moviedata.xml", "DPVS.dat"};
// the dependency lists, named after the level, each entry keyed by an attribute: a dependency by CRC (8239D908), a
// particle effect by Name
struct DependencyList {
    const char* suffix;
    const char* key;
};
constexpr DependencyList kDependencyLists[] = {{"_DepLoad.xml", " CRC=\""}, {"_DepLoadParticles.xml", " Name=\""}};
// the editor templates' lists, which the editor loads beside a campaign level's; FCIP.exe loads only the level's
// (825E5DA0, 825DC2E0)
constexpr const char* kEditorLists = "levels\\MET_Dependency\\MET_Dependency";
constexpr const char* kEntryIndent = "    <";  // a list's entries open at this indent, their children deeper
constexpr uint32_t kMaxGraphs = 256;
constexpr const char* kGraphManager = "GraphManager.bai";
constexpr uint32_t kGraphCount = 4;  // GraphManager.bai: version, then the graphs
constexpr const char* kLevelData = "LevelData.xml";
constexpr const char* kLevelInfo = "LevelInfo";
constexpr const char* kCampaignLevelKey = "CampaignLevel";  // names the level a custom level's assets come from
constexpr const char* kMissionFile = "Mission_Mission0.xml";
constexpr const char* kMovieFile = "moviedata.xml";
constexpr const char* kScriptName = "Mission.lua";
constexpr const char* kPaintFile = "textureinfo.ige";
constexpr const char* kCustomSuffix = "_Custom";  // a custom level never takes a campaign level's folder

// the object the editor's save takes (820C3968): the manager +0x58; the map changed flag it then clears
constexpr uint32_t kSaveObject = 0x58;
constexpr uint32_t kMapChanged = 0x8296B266;
// the mission object the save writes (82106B00 r3)
constexpr uint32_t kMissionObject = 0x8296CBC0;
// the editor's interface manager: the map's name, UTF-16
constexpr uint32_t kMapName = 0x68;
constexpr uint32_t kMapNameChars = 32;
// the 3D engine: the level folder's path, a C string, and its terrain
constexpr uint32_t kEngineLevelPath = 0x3A8;
constexpr uint32_t kEngineTerrain = 0x364;
// the terrain the editor's writers save (terrain.sdat 823255E8, LevelData 82109278), set by its save (82109C24)
constexpr uint32_t kSavedTerrain = 0x8296B2C8;
// the zip line ends the map save (82104C38) and zipline.dat (820F6A10) write, each with the cable to its other end;
// a ZipLine entity the solo editor gives a generic editor object, or one placed on its own, has none
constexpr uint32_t kZiplineCable = 0x80;
constexpr uint32_t kGenericObjectVtable = 0x8206AA30;
// the mounted weapons MountedWeapon.dat (820F6F10) writes, each with its weapon and the weapon's model, whose class
// names the type (820E7C78)
constexpr uint32_t kMountedWeapons = 0x828C5E7C;
constexpr uint32_t kMountedWeapon = 0x48;
constexpr uint32_t kWeaponModel = 0x2C;
// a std::vector: its first, last and end
constexpr uint32_t kVectorFirst = 0x04;
constexpr uint32_t kVectorLast = 0x08;
constexpr uint32_t kVectorEnd = 0x0C;
// a terrain sector (terrain +0x80, 0x160 each): its number, and its state, loaded once the streaming read it (82354EA8)
constexpr uint32_t kTerrainSectors = 0x80;
constexpr uint32_t kSectorBytes = 0x160;
constexpr uint32_t kSectorNumber = 0x78;  // int16
constexpr uint32_t kSectorState = 0x149;
constexpr uint8_t kSectorLoaded = 0x08;
constexpr uint32_t kSectorMetres = 64;
// the files the sector save (82353C18) writes from a loaded sector
constexpr const char* kSectorFiles[] = {"sdat\\sd%u.sdat", "sdat\\ShortRange%u.sdat", "sdat\\sd%u_diffuse_hi.xbt",
                                        "sdat\\ShortRange%u_shadow_hi.xbt"};
// terrain.sdat (823255E8): the side, big endian, then side x side vertices of 16 bytes
constexpr const char* kTerrainFile = "sdat\\terrain.sdat";
constexpr uint32_t kTerrainSide = 0x18;
constexpr uint32_t kTerrainVertices = 0x1C;
constexpr uint32_t kVertexBytes = 16;
constexpr uint32_t kLevelPathBytes = 0x40;
// the pack manager: set while a pack takes the files written
constexpr uint32_t kPacksWriting = 0x88;
// the editor's tools, each saved by its method +0x18 (tool, manager)
constexpr uint32_t kTools = 0x828C60A8;
constexpr uint32_t kToolsFirst = 0x04;
constexpr uint32_t kToolSave = 0x18;
// zipline.dat and MountedWeapon.dat, the names their writers take as level files
constexpr uint32_t kZiplineName = 0x8206B8A4;
constexpr uint32_t kMountedWeaponName = 0x8206B8B0;
// File_Open's modes
constexpr uint32_t kRead = 1;

// guest scratch: the XML text, a file, then a path
constexpr uint32_t kScratchString = 0x00;  // std::string: text +0x04, size +0x14, capacity +0x18
constexpr uint32_t kScratchFile = 0x20;
constexpr uint32_t kScratchFileBytes = 0x20;
constexpr uint32_t kScratchPath = kScratchFile + kScratchFileBytes;
constexpr uint32_t kScratchPathBytes = 0x80;
constexpr uint32_t kScratchKey = kScratchPath + kScratchPathBytes;
constexpr uint32_t kScratchBytes = kScratchKey + kScratchPathBytes;
constexpr uint32_t kStringText = 0x04;
constexpr uint32_t kStringSize = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kStringInlineCapacity = 0x0F;

uint32_t g_scratch = 0;
bool g_saving = false;  // SaveCustomLevel runs
uint32_t g_buffer = 0;  // a copied file
uint32_t g_buffer_bytes = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

bool Saving() { return g_solo && !BaseLevel().empty(); }

uint32_t Text(uint32_t at, const std::string& text) { return WriteText(g_scratch + at, text, kScratchPathBytes); }

// the map's name as the save page took it, the custom level's folder name
std::string MapName() {
    uint32_t manager = R32(SYM_Editor_Manager_pointer);
    std::string name;
    for (uint32_t i = 0; i < kMapNameChars; i++) {
        uint16_t c = R16(manager + kMapName + i * 2);
        if (!c) break;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        name += keep ? char(c) : '_';
    }
    if (name.empty()) name = BaseLevel();
    return IsCampaignLevel(name) ? name + kCustomSuffix : name;
}

fs::path LevelFolder() { return fcip::CustomLevelFolder(MapName()); }

void WriteHostFile(const std::string& name, const void* data, size_t size) {
    fs::path path = LevelFolder() / name;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(size));
}

void WriteXml(PPCContext& ctx, const std::string& name, uint32_t node) {
    uint32_t str = g_scratch + kScratchString;
    W32(str + kStringSize, 0);
    W32(str + kStringCapacity, kStringInlineCapacity);
    W8(str + kStringText, 0);
    CallGuest(ctx, Method(node, kNodeGetXml), {node, str, 0});
    uint32_t text = str + kStringText;
    if (R32(str + kStringCapacity) > kStringInlineCapacity) text = R32(text);
    WriteHostFile(name, HostPtr(text), R32(str + kStringSize));
    CallGuest(ctx, SYM_String_Destroy, {str});
}

uint32_t Buffer(uint32_t bytes) {
    if (bytes > g_buffer_bytes) {
        g_buffer = mem::AllocSystem(bytes);
        g_buffer_bytes = g_buffer ? bytes : 0;
    }
    return g_buffer;
}

// a file as the game reads it, through the packs; false when there is none, which File_Open would stop the game for
bool ReadGameFile(PPCContext& ctx, const std::string& file, std::vector<uint8_t>& out) {
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    uint32_t path = Text(kScratchPath, file);
    if (!(CallGuest(ctx, SYM_File_Exists, {packs, path}) & 0xFF)) return false;
    uint32_t handle = g_scratch + kScratchFile;
    CallGuest(ctx, SYM_File_Open, {handle, packs, path, kRead, 0, 0});
    bool open = CallGuest(ctx, SYM_File_IsOpen, {handle}) & 0xFF;
    if (open) {
        uint32_t size = uint32_t(CallGuest(ctx, SYM_File_Size, {packs, handle}));
        uint32_t data = Buffer(size);
        if (data) {
            CallGuest(ctx, SYM_File_Read, {packs, data, size, handle});
            const uint8_t* bytes = static_cast<const uint8_t*>(HostPtr(data));
            out.assign(bytes, bytes + size);
        }
        CallGuest(ctx, SYM_File_Close, {packs, handle});
    }
    CallGuest(ctx, SYM_File_Destroy, {handle});
    return open;
}

// a file of the campaign level, read from its pack, into the custom folder as name
void CopyFile(PPCContext& ctx, const std::string& level, const std::string& file, const std::string& name) {
    std::vector<uint8_t> data;
    if (ReadGameFile(ctx, "levels\\" + level + "\\" + file, data)) WriteHostFile(name, data.data(), data.size());
}

std::vector<std::string> Lines(const std::vector<uint8_t>& text) {
    std::vector<std::string> lines(1);
    for (uint8_t c : text) {
        lines.back() += char(c);
        if (c == '\n') lines.emplace_back();
    }
    return lines;
}

bool OpensEntry(const std::string& line) {
    size_t indent = std::char_traits<char>::length(kEntryIndent);
    return line.compare(0, indent, kEntryIndent) == 0 && line.size() > indent && line[indent] != ' ' &&
           line[indent] != '/';
}

std::string KeyOf(const std::string& line, const std::string& key) {
    size_t at = line.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    return line.substr(at, line.find('"', at) - at);
}

// the level's list with every editor template entry it lacks put before its closing tag
std::vector<uint8_t> MergeLists(const std::vector<uint8_t>& level, const std::vector<uint8_t>& editor,
                                const std::string& key) {
    std::set<std::string> have;
    for (const std::string& line : Lines(level)) {
        if (OpensEntry(line)) have.insert(KeyOf(line, key));
    }
    std::string added;
    bool copy = false;
    for (const std::string& line : Lines(editor)) {
        if (OpensEntry(line)) copy = have.insert(KeyOf(line, key)).second;
        else if (line.compare(0, 1, "<") == 0) copy = false;  // the header and the root's own tags
        if (copy) added += line;
    }
    std::string text(level.begin(), level.end());
    size_t close = text.rfind("</");
    if (close == std::string::npos) return level;
    text.insert(close, added);
    return {text.begin(), text.end()};
}

void CopyCampaignFiles(PPCContext& ctx) {
    std::string level = BaseLevel();
    for (const char* file : kCopiedFiles) CopyFile(ctx, level, file, file);
    std::vector<uint8_t> vegbank;
    if (ReadGameFile(ctx, "levels\\" + level + "\\vegbank.dat", vegbank)) {
        for (const std::string& file : SpriteFiles(vegbank)) CopyFile(ctx, level, file, file);
    }
    WriteDependencyLists(ctx, level, LevelFolder(), MapName());
}

// an entity's type as the campaign's missions place it: its archetype, else its class and model
std::string EntityType(const std::string& entity) {
    std::string archetype = KeyOf(entity, " tplCreatureType=\"");
    if (!archetype.empty()) return archetype;
    return KeyOf(entity, " EntityClass=\"") + "|" + KeyOf(entity, " object_Model=\"");
}

struct PlacedEntity {
    std::string name;
    std::string type;
};

// a mission's entities, each from its "<Entity" to its "/>" or "</Entity>"
std::vector<PlacedEntity> MissionEntities(const std::string& mission) {
    constexpr const char* kOpen = "<Entity ";
    constexpr const char* kClose = "</Entity>";
    std::vector<PlacedEntity> out;
    for (size_t at = mission.find(kOpen); at != std::string::npos; at = mission.find(kOpen, at + 1)) {
        size_t tag = mission.find('>', at);
        if (tag == std::string::npos) break;
        size_t end = mission[tag - 1] == '/' ? tag : mission.find(kClose, tag);
        if (end == std::string::npos) break;
        std::string entity = mission.substr(at, end - at);
        out.push_back({KeyOf(entity, " Name=\""), EntityType(entity)});
    }
    return out;
}

// a dependency list's entries by their key's value, each its lines up to the next entry or the root's end
std::map<std::string, std::string> ListEntries(const std::vector<uint8_t>& list, const std::string& key) {
    std::map<std::string, std::string> entries;
    std::string* entry = nullptr;
    for (const std::string& line : Lines(list)) {
        if (OpensEntry(line)) entry = &entries[KeyOf(line, key)];
        else if (line.compare(0, 1, "<") == 0) entry = nullptr;
        if (entry) *entry += line;
    }
    return entries;
}

// an entry's first line with one attribute's value replaced
void SetEntryKey(std::string& entry, const std::string& key, const std::string& value) {
    size_t at = entry.find(key);
    if (at == std::string::npos || at > entry.find('\n')) return;
    at += key.size();
    entry.replace(at, entry.find('"', at) - at, value);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<uint8_t> HostFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void AppendEntries(std::vector<uint8_t>& list, const std::string& added) {
    std::string text(list.begin(), list.end());
    size_t close = text.rfind("</");
    if (close == std::string::npos || added.empty()) return;
    text.insert(close, added);
    list.assign(text.begin(), text.end());
}

// a campaign level's mission, its entities by type, and its dependency lists, read once per save
struct CampaignSource {
    std::string level;
    std::map<std::string, std::string> names;  // type, the first entity of it
    std::map<std::string, std::string> entries;
    std::map<std::string, std::string> particles;
};

CampaignSource ReadSource(PPCContext& ctx, const std::string& level) {
    CampaignSource source{level, {}, {}, {}};
    std::vector<uint8_t> mission;
    if (!ReadGameFile(ctx, "levels\\" + level + "\\" + kMissionFile, mission)) return source;
    for (const PlacedEntity& e : MissionEntities({mission.begin(), mission.end()})) {
        source.names.emplace(e.type, e.name);
    }
    std::vector<uint8_t> list;
    std::string lists = "levels\\" + level + "\\" + level;
    if (ReadGameFile(ctx, lists + kDependencyLists[0].suffix, list)) {
        source.entries = ListEntries(list, " Name=\"");
    }
    if (ReadGameFile(ctx, lists + kDependencyLists[1].suffix, list)) source.particles = ListEntries(list, " Name=\"");
    return source;
}

// the particle effects an entry loads, by name
std::vector<std::string> EntryEffects(const std::string& entry) {
    constexpr const char* kEffect = " Type=\"10\" Name=\"";
    std::vector<std::string> out;
    for (size_t at = entry.find(kEffect); at != std::string::npos; at = entry.find(kEffect, at + 1)) {
        out.push_back(KeyOf(entry.substr(at), " Name=\""));
    }
    return out;
}

// the game loads an entity's models from the dependency list entry keyed by crc32("\\" + its lowercase name)
// (8239D908), so every entity placed in the editor gets the entry of a campaign entity of its type, renamed, and the
// particle entries it names; FCIP.exe packs the files they list from whichever campaign level holds them
void AddPlacedDependencies(PPCContext& ctx) {
    fs::path folder = LevelFolder();
    fs::path own_path = folder / (MapName() + kDependencyLists[0].suffix);
    fs::path particles_path = folder / (MapName() + kDependencyLists[1].suffix);
    std::vector<uint8_t> own = HostFile(own_path);
    std::vector<uint8_t> particles = HostFile(particles_path);
    std::set<std::string> listed;
    for (const std::string& line : Lines(own)) {
        if (OpensEntry(line)) listed.insert(KeyOf(line, kDependencyLists[0].key));
    }
    std::set<std::string> effects;
    for (const std::string& line : Lines(particles)) {
        if (OpensEntry(line)) effects.insert(KeyOf(line, kDependencyLists[1].key));
    }
    std::vector<std::string> levels = {BaseLevel()};
    for (const char* level : kCampaignLevels) {
        if (level != BaseLevel()) levels.push_back(level);
    }
    std::vector<CampaignSource> sources;
    std::string added;
    std::string added_effects;
    uint32_t placed = 0;
    uint32_t unknown = 0;
    std::vector<uint8_t> mission = HostFile(folder / kMissionFile);
    for (const PlacedEntity& e : MissionEntities({mission.begin(), mission.end()})) {
        std::string crc = std::to_string(fcip::Crc32("\\" + Lower(e.name)));
        if (e.name.empty() || listed.count(crc)) continue;
        const CampaignSource* from = nullptr;
        for (size_t i = 0; !from && i < levels.size(); i++) {
            if (i == sources.size()) sources.push_back(ReadSource(ctx, levels[i]));
            auto named = sources[i].names.find(e.type);
            if (named != sources[i].names.end() && sources[i].entries.count(named->second)) from = &sources[i];
        }
        if (!from) {
            unknown++;
            continue;
        }
        std::string entry = from->entries.at(from->names.at(e.type));
        SetEntryKey(entry, " Name=\"", e.name);
        SetEntryKey(entry, kDependencyLists[0].key, crc);
        added += entry;
        listed.insert(crc);
        placed++;
        for (const std::string& effect : EntryEffects(entry)) {
            auto particle = from->particles.find(effect);
            if (particle != from->particles.end() && effects.insert(effect).second) added_effects += particle->second;
        }
    }
    AppendEntries(own, added);
    AppendEntries(particles, added_effects);
    for (const auto& [path, list] : {std::pair(own_path, &own), std::pair(particles_path, &particles)}) {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(list->data()), std::streamsize(list->size()));
    }
    LOGR("level editor: %u placed entities given dependencies, %u of a type no campaign level places", placed, unknown);
}

// the sequences as edited, over the campaign's copy
void WriteMovie(PPCContext& ctx) {
    if (uint32_t movie = KeptMovie()) WriteXml(ctx, kMovieFile, movie);
}

// the graphs the level loaded, from where the AI system read them (8228CF00, "%s/%s")
std::vector<NavFile> LoadedGraphs(PPCContext& ctx) {
    std::vector<NavFile> graphs;
    std::string folder = LoadedNavFolder();
    NavFile manager;
    bool read = !folder.empty() && ReadGameFile(ctx, folder + "/" + kGraphManager, manager);
    if (!read || manager.size() < kGraphCount + 2) return graphs;
    uint32_t count = std::min<uint32_t>(manager[kGraphCount] | manager[kGraphCount + 1] << 8, kMaxGraphs);
    for (uint32_t i = 0; i < count; i++) {
        NavFile graph;
        ReadGameFile(ctx, folder + "/Graph" + std::to_string(i) + ".bai", graph);
        graphs.push_back(std::move(graph));
    }
    return graphs;
}

void AddAreas(const AiObjects& objects, std::vector<NavArea>* areas, std::vector<NavRing>* forbidden) {
    for (const AiObject& o : objects.objects) {
        NavRing ring(o.points.begin(), o.points.end());
        if (o.type == "EnclosingArea" && areas) areas->push_back({o.name, ring});
        if (o.type == "ForbiddenArea") forbidden->push_back(ring);
    }
}

void WriteNavigation(PPCContext& ctx) {
    std::vector<NavFile> graphs = RebuildNavGraphs(ctx);
    for (size_t i = 0; i < graphs.size(); i++) {
        WriteHostFile("Graph" + std::to_string(i) + ".bai", graphs[i].data(), graphs[i].size());
    }
    NavFile manager = NavManagerFile(uint32_t(graphs.size()));
    WriteHostFile(kGraphManager, manager.data(), manager.size());
    LOGR("level editor: %zu AI navigation graphs written", graphs.size());
}

// the campaign level's LevelData.xml, naming the campaign level
void WriteLevelData(PPCContext& ctx) {
    uint32_t data = KeptLevelData();
    if (!data) return;
    uint32_t info = 0;
    for (uint32_t i = 0; i < ChildCount(data) && !info; i++) {
        if (GuestString(ChildAt(data, i) + kNodeTag) == kLevelInfo) info = ChildAt(data, i);
    }
    if (info) {
        uint32_t key = Text(kScratchKey, kCampaignLevelKey);
        CallGuest(ctx, Method(info, kNodeSetAttr), {info, key, Text(kScratchPath, BaseLevel())});
    }
    WriteXml(ctx, kLevelData, data);
}

// the campaign's mission with the editor's entities and the edited AI objects
void WriteMission(PPCContext& ctx) {
    uint32_t mission = MissionToSave(ctx, kMissionObject);
    if (!mission) return;
    WriteXml(ctx, kMissionFile, mission);
    CallGuest(ctx, Method(mission, kNodeRelease), {mission});
}

// a new level's script: the campaign's functions are kept and called, so it starts by changing nothing; Campaign is
// taken once, so the editor running the script again on a change keeps the campaign's own
std::string ScriptTemplate(const std::string& name) {
    return "-- " + name + ": runs after the campaign level's mission script, whose table is Mission.\n"
           "-- A function set here replaces the campaign's; call the one kept in Campaign to keep its behaviour.\n"
           "\n"
           "Campaign = Campaign or { OnInit = Mission.OnInit }\n"
           "\n"
           "function Mission:OnInit()\n"
           "\tif Campaign.OnInit then Campaign.OnInit(self) end\n"
           "end\n";
}

void EnsureScript() {
    fs::path path = OwnScriptPath();
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream(path, std::ios::binary) << ScriptTemplate(MapName());
    LOGR("level editor: %s written", path.string().c_str());
}

// a text in a file replaced throughout, the file untouched when it has none
void ReplaceInFile(const fs::path& path, const std::string& from, const std::string& to) {
    std::ifstream in(path, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(in), {});
    in.close();
    bool found = false;
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) {
        text.replace(at, from.size(), to);
        found = true;
    }
    if (found) std::ofstream(path, std::ios::binary) << text;
}

// a custom level saved under another name: what it loaded still names the folder it started from, its mission's
// script, LevelData's Name and its string table; a campaign level's name the campaign's own
bool Renamed() { return StartedLevel() != BaseLevel() && StartedLevel() != MapName(); }

void RenameOwnReferences() {
    std::string from = StartedLevel();
    std::string to = MapName();
    ReplaceInFile(LevelFolder() / kMissionFile, "\\Levels\\" + from + "\\", "\\Levels\\" + to + "\\");
    ReplaceInFile(LevelFolder() / kLevelData, "Name=\"" + from + "\"", "Name=\"" + to + "\"");
}

// Save As: the files of the folder the level saved to, under the new name; its pack is made again for it, and its
// script loads the string table named after it
void CopyOwnFiles(const fs::path& from, const std::string& old) {
    std::string name = MapName();
    fs::path to = LevelFolder();
    std::error_code ec;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(from, ec)) {
        if (!e.is_regular_file(ec)) continue;
        fs::path rel = fs::relative(e.path(), from, ec);
        std::string file = rel.filename().string();
        if (file == old + ".dat" || file == old + ".fat") continue;
        if (rel.parent_path().empty() && file.starts_with(old)) file = name + file.substr(old.size());
        fs::path out = to / rel.parent_path() / file;
        fs::create_directories(out.parent_path(), ec);
        fs::copy_file(e.path(), out, fs::copy_options::overwrite_existing, ec);
    }
    ReplaceInFile(to / kScriptName, "\"" + old + ".xml\"", "\"" + name + ".xml\"");
}

// a file of the level as it loaded, from its pack or its folder
bool ReadLevelFile(PPCContext& ctx, const std::string& name, std::vector<uint8_t>& out) {
    return ReadGameFile(ctx, "levels\\" + StartedLevel() + "\\" + name, out);
}

// the save goes to the folder the level loaded from, which already holds what it has not changed
bool SavingInPlace() {
    std::error_code ec;
    return fs::equivalent(fcip::CustomLevelFolder(StartedLevel()), LevelFolder(), ec);
}

uint32_t Sector(uint32_t index) {
    uint32_t terrain = R32(R32(SYM_C3DEngine_pointer) + kEngineTerrain);
    return R32(terrain + kTerrainSectors) + index * kSectorBytes;
}

bool SectorLoaded(uint32_t index) { return R8(Sector(index) + kSectorState) & kSectorLoaded; }

// a sector the streaming has not loaded has no data to save; its files stay as the level has them
void KeepSectorFiles(PPCContext& ctx, uint32_t number) {
    if (SavingInPlace()) return;
    for (const char* pattern : kSectorFiles) {
        char name[64];
        std::snprintf(name, sizeof(name), pattern, number);
        std::vector<uint8_t> data;
        if (ReadLevelFile(ctx, name, data)) WriteHostFile(name, data.data(), data.size());
    }
}

uint32_t Be32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}

// terrain.sdat as saved takes its vertices from loaded sectors only (823255E8); the rest come from the level's own
void KeepUnloadedTerrain(const std::vector<uint8_t>& old) {
    fs::path path = LevelFolder() / kTerrainFile;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> now(std::istreambuf_iterator<char>(in), {});
    in.close();
    if (now.size() <= kTerrainVertices || now.size() != old.size()) return;
    uint32_t side = Be32(now, kTerrainSide);
    uint32_t sectors = R32(SYM_Terrain_SectorsPerSide);
    if (side < 2 || size_t(side) * side * kVertexBytes + kTerrainVertices != now.size()) return;
    std::vector<bool> loaded(size_t(sectors) * sectors);
    for (uint32_t i = 0; i < loaded.size(); i++) loaded[i] = SectorLoaded(i);
    for (uint32_t y = 0; y < side; y++) {
        for (uint32_t x = 0; x < side; x++) {
            uint32_t sector = std::min(y, side - 2) / kSectorMetres * sectors + std::min(x, side - 2) / kSectorMetres;
            if (sector >= loaded.size() || loaded[sector]) continue;
            size_t at = kTerrainVertices + (size_t(y) * side + x) * kVertexBytes;
            std::copy_n(old.begin() + at, kVertexBytes, now.begin() + at);
        }
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(now.data()), std::streamsize(now.size()));
}

// an editor list holding only the objects its writers can save while the save runs
struct KeptList {
    uint32_t list;
    bool (*whole)(uint32_t object);
    uint32_t buffer = 0;
    uint32_t bytes = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t end = 0;
};

bool Cabled(uint32_t end) { return R32(end) != kGenericObjectVtable && R32(end + kZiplineCable); }

bool Armed(uint32_t mounted) {
    uint32_t weapon = R32(mounted + kMountedWeapon);
    return R32(mounted) != kGenericObjectVtable && weapon && R32(weapon + kWeaponModel);
}

KeptList g_ziplines{SYM_Ziplines, Cabled};
KeptList g_mounted{kMountedWeapons, Armed};

void Keep(KeptList& kept) {
    kept.first = R32(kept.list + kVectorFirst);
    kept.last = R32(kept.list + kVectorLast);
    kept.end = R32(kept.list + kVectorEnd);
    std::vector<uint32_t> whole;
    for (uint32_t at = kept.first; at && at < kept.last; at += 4)
        if (kept.whole(R32(at))) whole.push_back(R32(at));
    if (whole.size() == (kept.last - kept.first) / 4) return;
    uint32_t bytes = uint32_t(whole.size() * 4);
    if (bytes > kept.bytes) {
        kept.buffer = mem::AllocSystem(bytes);
        kept.bytes = kept.buffer ? bytes : 0;
    }
    if (!kept.buffer && bytes) return;
    for (size_t i = 0; i < whole.size(); i++) W32(kept.buffer + uint32_t(i * 4), whole[i]);
    W32(kept.list + kVectorFirst, kept.buffer);
    W32(kept.list + kVectorLast, kept.buffer + bytes);
    W32(kept.list + kVectorEnd, kept.buffer + bytes);
}

void Restore(const KeptList& kept) {
    W32(kept.list + kVectorFirst, kept.first);
    W32(kept.list + kVectorLast, kept.last);
    W32(kept.list + kVectorEnd, kept.end);
}

// the editor's own writers, their files going to the level path
void WriteEditorFiles(PPCContext& ctx, uint32_t manager) {
    uint32_t count = uint32_t(CallGuest(ctx, SYM_Tools_Count, {}));
    uint32_t first = R32(kTools + kToolsFirst);
    for (uint32_t i = 0; first && i < count; i++) {
        uint32_t tool = R32(first + i * 4);
        CallGuest(ctx, Method(tool, kToolSave), {tool, manager});
    }
    CallGuest(ctx, SYM_Terrain_Save, {manager});
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    CallGuest(ctx, SYM_Zipline_Save, {uint32_t(CallGuest(ctx, SYM_Engine_LevelFilePath, {engine, kZiplineName, 0}))});
    uint32_t mounted = uint32_t(CallGuest(ctx, SYM_Engine_LevelFilePath, {engine, kMountedWeaponName, 0}));
    CallGuest(ctx, SYM_MountedWeapon_Save, {mounted});
}

}  // namespace

void fcip::editor::SaveCustomLevel(PPCContext& ctx, uint32_t manager) {
    if (!EnsureScratch()) return;
    std::error_code ec;
    fs::create_directories(LevelFolder() / "sdat", ec);
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    W32(kSavedTerrain, R32(engine + kEngineTerrain));
    Keep(g_ziplines);
    Keep(g_mounted);
    std::string level_path = ReadCString(engine + kEngineLevelPath);
    WriteText(engine + kEngineLevelPath, MapName(), kLevelPathBytes);
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    uint8_t writing = R8(packs + kPacksWriting);
    W8(packs + kPacksWriting, 0);
    CopyCampaignFiles(ctx);
    WriteMovie(ctx);
    WriteLevelData(ctx);
    WriteMission(ctx);
    AddPlacedDependencies(ctx);
    StringTableFile table = SavedStringTable();
    if (Renamed()) {
        RenameOwnReferences();
        if (table.name == StartedLevel() + ".xml") table.name = MapName() + ".xml";
    }
    if (!table.name.empty()) WriteHostFile(table.name, table.text.data(), table.text.size());
    WriteNavigation(ctx);
    std::vector<uint8_t> terrain;
    ReadLevelFile(ctx, kTerrainFile, terrain);
    g_saving = true;
    WriteEditorFiles(ctx, manager);
    g_saving = false;
    KeepUnloadedTerrain(terrain);
    std::vector<uint8_t> paint = PaintLayerBytes();
    WriteHostFile(kPaintFile, paint.data(), paint.size());
    EnsureScript();
    W8(packs + kPacksWriting, writing);
    WriteText(engine + kEngineLevelPath, level_path, kLevelPathBytes);
    Restore(g_mounted);
    Restore(g_ziplines);
    LOGR("level editor: %s saved as %s", BaseLevel().c_str(), LevelFolder().string().c_str());
}

void fcip::editor::SaveCustomLevelAs(PPCContext& ctx, const std::string& name) {
    StopPreview(ctx);
    std::string old = MapName();
    fs::path from = LevelFolder();
    uint32_t manager = R32(SYM_Editor_Manager_pointer);
    uint32_t n = uint32_t(std::min<size_t>(name.size(), kMapNameChars - 1));
    for (uint32_t i = 0; i < n; i++) W16(manager + kMapName + i * 2, uint8_t(name[i]));
    W16(manager + kMapName + n * 2, 0);
    std::error_code ec;
    if (fs::is_directory(from, ec) && !fs::equivalent(from, LevelFolder(), ec)) CopyOwnFiles(from, old);
    SaveCustomLevel(ctx, manager + kSaveObject);
    W8(kMapChanged, 0);
}

std::filesystem::path fcip::editor::OwnScriptPath() { return LevelFolder() / kScriptName; }

// the base level's dependency lists with the editor templates' entries, so FCIP.exe preloads what the editor did; the
// editor's start needs the level's own (8239D908 reads it through 82367828, which stops the game on a missing file)
void fcip::editor::WriteDependencyLists(PPCContext& ctx, const std::string& base, const fs::path& folder,
                                        const std::string& name) {
    if (!EnsureScratch()) return;
    for (const DependencyList& list : kDependencyLists) {
        std::vector<uint8_t> own;
        std::vector<uint8_t> editor;
        bool has_own = ReadGameFile(ctx, "levels\\" + base + "\\" + base + list.suffix, own);
        bool has_editor = ReadGameFile(ctx, std::string(kEditorLists) + list.suffix, editor);
        if (!has_own && !has_editor) continue;
        std::vector<uint8_t> merged = !has_own ? editor : has_editor ? MergeLists(own, editor, list.key) : own;
        std::ofstream(folder / (name + list.suffix), std::ios::binary)
            .write(reinterpret_cast<const char*>(merged.data()), std::streamsize(merged.size()));
    }
}

// the AI navigation cut again for the level's areas as they are now
std::vector<NavFile> fcip::editor::RebuildNavGraphs(PPCContext& ctx) {
    if (!EnsureScratch()) return {};
    NavInput in;
    in.loaded_graphs = LoadedGraphs(ctx);
    std::vector<NavArea> loaded_areas;
    AddAreas(*ReadLoadedAiObjects(), &loaded_areas, &in.loaded_forbidden);
    for (const NavArea& a : loaded_areas) in.loaded_areas.push_back(a.name);
    AddAreas(*ReadAiObjects(), &in.areas, &in.forbidden);
    // a level with no graphs yet, a new one, has no static geometry cut: its brushes are cut as obstacles
    if (in.loaded_graphs.empty()) {
        for (const BrushFootprint& b : BrushFootprints()) {
            in.forbidden.push_back({{b.min_x, b.min_y, 0}, {b.max_x, b.min_y, 0}, {b.max_x, b.max_y, 0},
                                    {b.min_x, b.max_y, 0}});
        }
    }
    return BuildNavGraphs(in, [&](float x, float y) { return TerrainHeightAt(ctx, x, y); });
}

bool fcip::editor::ReadPackFile(PPCContext& ctx, const std::string& file, std::vector<uint8_t>& out) {
    return EnsureScratch() && ReadGameFile(ctx, file, out);
}

PPC_EXTERN_FUNC(__imp__sub_82109BF8);
PPC_EXTERN_FUNC(__imp__sub_82353C18);

// a sector saves (r3 the sector); one the streaming has not loaded keeps the files the level has
PPC_FUNC(sub_82353C18) {
    uint32_t sector = uint32_t(ctx.r[3]);
    if (!g_saving || (R8(sector + kSectorState) & kSectorLoaded)) return __imp__sub_82353C18(ctx, base);
    KeepSectorFiles(ctx, R16(sector + kSectorNumber));
    ctx.r[3] = 1;
}

// the editor's save (r3 the manager); a campaign level saves as its own folder. A sequence preview's placements are
// put back first, so entities save where they stand
PPC_FUNC(sub_82109BF8) {
    if (g_solo) StopPreview(ctx);
    if (!Saving()) return __imp__sub_82109BF8(ctx, base);
    // a new level saves once Save As names it, which this save opens
    if (LevelUnsaved()) {
        AskSaveAs();
        ctx.r[3] = 1;
        return;
    }
    SaveCustomLevel(ctx, uint32_t(ctx.r[3]));
    ctx.r[3] = 1;
}
