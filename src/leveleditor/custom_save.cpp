// ige.xex, the solo level editor: a campaign level saves as a custom level, its own folder levels\<name>\ beside the
// executable, which is laid over game:\Levels. The editor's own save (82109BF8) puts a map into a user map, a 4 MB blob with room
// for 255 files, which a campaign level's 550 files and 40 MB do not fit. Its writers make real files when no pack
// takes them (File_Open 8234EF80), so the save runs them with the engine's level path on the custom level: terrain,
// sectors, tools, ziplines and mounted weapons. The rest are written here: the campaign's level files the editor does
// not write, its LevelData.xml naming the campaign level, the mission with the editor's entities and AI objects, the
// paint layers, and a first Mission.lua that runs after the campaign's mission script.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

namespace fs = std::filesystem;

// the campaign level's files the editor's save does not write, copied from its pack; Graph%d.bai until one is missing
constexpr const char* kCopiedFiles[] = {"Brush.lst", "vegbank.dat", "moviedata.xml", "GraphManager.bai"};
// the dependency lists, named after the level
constexpr const char* kDependencyLists[] = {"_DepLoad.xml", "_DepLoadParticles.xml"};
constexpr uint32_t kMaxGraphs = 256;
constexpr const char* kLevelData = "LevelData.xml";
constexpr const char* kLevelInfo = "LevelInfo";
constexpr const char* kCampaignLevelKey = "CampaignLevel";  // names the level a custom level's assets come from
constexpr const char* kMissionFile = "Mission_Mission0.xml";
constexpr const char* kScriptName = "Mission.lua";
constexpr const char* kPaintFile = "textureinfo.ige";
constexpr const char* kCustomSuffix = "_Custom";  // a custom level never takes a campaign level's folder

// the mission object the save writes (82106B00 r3)
constexpr uint32_t kMissionObject = 0x8296CBC0;
// the editor's interface manager: the map's name, UTF-16
constexpr uint32_t kMapName = 0x68;
constexpr uint32_t kMapNameChars = 32;
// the 3D engine: the level folder's path, a C string
constexpr uint32_t kEngineLevelPath = 0x3A8;
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
uint32_t g_buffer = 0;  // a copied file
uint32_t g_buffer_bytes = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

bool Saving() { return g_solo && !CampaignLevel().empty(); }

uint32_t Text(uint32_t at, const std::string& text) { return WriteText(g_scratch + at, text, kScratchPathBytes); }

bool IsCampaignLevel(const std::string& name) {
    auto same = [](char a, char b) { return std::toupper(uint8_t(a)) == std::toupper(uint8_t(b)); };
    for (std::string level : kCampaignLevels) {
        if (std::equal(name.begin(), name.end(), level.begin(), level.end(), same)) return true;
    }
    return false;
}

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
    if (name.empty()) name = CampaignLevel();
    return IsCampaignLevel(name) ? name + kCustomSuffix : name;
}

fs::path LevelFolder() { return app::ExeDir() / "levels" / MapName(); }

void WriteHostFile(const std::string& name, const void* data, size_t size) {
    std::ofstream(LevelFolder() / name, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(size));
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

// a file of the campaign level, read from its pack, into the custom folder as name; false when the campaign has none
bool CopyFile(PPCContext& ctx, const std::string& level, const std::string& file, const std::string& name) {
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    uint32_t handle = g_scratch + kScratchFile;
    uint32_t path = Text(kScratchPath, "levels\\" + level + "\\" + file);
    CallGuest(ctx, SYM_File_Open, {handle, packs, path, kRead, 0, 0});
    bool open = CallGuest(ctx, SYM_File_IsOpen, {handle}) & 0xFF;
    if (open) {
        uint32_t size = uint32_t(CallGuest(ctx, SYM_File_Size, {packs, handle}));
        uint32_t data = Buffer(size);
        if (data) {
            CallGuest(ctx, SYM_File_Read, {packs, data, size, handle});
            WriteHostFile(name, HostPtr(data), size);
        }
        CallGuest(ctx, SYM_File_Close, {packs, handle});
    }
    CallGuest(ctx, SYM_File_Destroy, {handle});
    return open;
}

void CopyCampaignFiles(PPCContext& ctx) {
    std::string level = CampaignLevel();
    for (const char* file : kCopiedFiles) CopyFile(ctx, level, file, file);
    for (const char* list : kDependencyLists) CopyFile(ctx, level, level + list, MapName() + list);
    uint32_t graph = 0;
    while (graph < kMaxGraphs) {
        std::string file = "Graph" + std::to_string(graph) + ".bai";
        if (!CopyFile(ctx, level, file, file)) break;
        graph++;
    }
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
        CallGuest(ctx, Method(info, kNodeSetAttr), {info, key, Text(kScratchPath, CampaignLevel())});
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

void SaveCustomLevel(PPCContext& ctx, uint32_t manager) {
    std::error_code ec;
    fs::create_directories(LevelFolder() / "sdat", ec);
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    std::string level_path = ReadCString(engine + kEngineLevelPath);
    WriteText(engine + kEngineLevelPath, MapName(), kLevelPathBytes);
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    uint8_t writing = R8(packs + kPacksWriting);
    W8(packs + kPacksWriting, 0);
    CopyCampaignFiles(ctx);
    WriteLevelData(ctx);
    WriteMission(ctx);
    WriteEditorFiles(ctx, manager);
    std::vector<uint8_t> paint = PaintLayerBytes();
    WriteHostFile(kPaintFile, paint.data(), paint.size());
    EnsureScript();
    W8(packs + kPacksWriting, writing);
    WriteText(engine + kEngineLevelPath, level_path, kLevelPathBytes);
    LOGR("level editor: %s saved as %s", CampaignLevel().c_str(), LevelFolder().string().c_str());
}

}  // namespace

std::filesystem::path fcip::editor::OwnScriptPath() { return LevelFolder() / kScriptName; }

PPC_EXTERN_FUNC(__imp__sub_82109BF8);

// the editor's save (r3 the manager); a campaign level saves as its own folder
PPC_FUNC(sub_82109BF8) {
    if (!Saving() || !EnsureScratch()) return __imp__sub_82109BF8(ctx, base);
    SaveCustomLevel(ctx, uint32_t(ctx.r[3]));
    ctx.r[3] = 1;
}
