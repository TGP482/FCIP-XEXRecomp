// ige.xex, the solo level editor: a campaign level saves as a custom level through the editor's own save (82109BF8),
// which writes the terrain, sectors, paint and objects into the user map's pack. Three of its steps write template map
// data, so a campaign level gets its own: the campaign's level files the editor does not write, its LevelData.xml, and
// its mission with the editor's entities and the edited AI objects. The level's own script, levels\<name>.lua beside
// the executable, goes in as Mission.lua; it runs after the campaign's mission script.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "app/app.h"
#include "core/log.h"
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
constexpr const char* kDependencies = "_DepLoad.xml";
constexpr uint32_t kMaxGraphs = 256;
constexpr const char* kLevelData = "LevelData.xml";
constexpr const char* kLevelInfo = "LevelInfo";
constexpr const char* kCampaignLevelKey = "CampaignLevel";  // names the level a custom level's assets come from
constexpr const char* kScriptName = "Mission.lua";

// the mission object (0x8296CBC0): the file it saves to, std::string
constexpr uint32_t kMissionFile = 0x80;
// the editor's interface manager: the map's name, UTF-16
constexpr uint32_t kMapName = 0x68;
constexpr uint32_t kMapNameChars = 32;
// File_Open's modes
constexpr uint32_t kWrite = 2;

// guest scratch: the XML text, a file, then texts
constexpr uint32_t kScratchString = 0x00;  // std::string: text +0x04, size +0x14, capacity +0x18
constexpr uint32_t kScratchFile = 0x20;
constexpr uint32_t kScratchFileBytes = 0x20;
constexpr uint32_t kScratchPack = kScratchFile + kScratchFileBytes;
constexpr uint32_t kScratchPathBytes = 0x80;
constexpr uint32_t kScratchSource = kScratchPack + kScratchPathBytes;
constexpr uint32_t kScratchName = kScratchSource + kScratchPathBytes;
constexpr uint32_t kScratchBytes = kScratchName + kScratchPathBytes;
constexpr uint32_t kStringText = 0x04;
constexpr uint32_t kStringSize = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kStringInlineCapacity = 0x0F;

uint32_t g_scratch = 0;
uint32_t g_buffer = 0;  // the script's text
uint32_t g_buffer_bytes = 0;

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

bool Saving() { return g_solo && !CampaignLevel().empty(); }

uint32_t Text(uint32_t at, const std::string& text) { return WriteText(g_scratch + at, text, kScratchPathBytes); }

// a file of the level being saved, its folder the user map's pack
void WriteLevelFile(PPCContext& ctx, const std::string& name, uint32_t data, uint32_t size) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t path = uint32_t(CallGuest(ctx, SYM_Engine_LevelFilePath, {engine, Text(kScratchName, name), 0}));
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    uint32_t file = g_scratch + kScratchFile;
    CallGuest(ctx, SYM_File_Open, {file, packs, path, kWrite, 0, 0});
    CallGuest(ctx, SYM_File_Write, {packs, data, size, file});
    CallGuest(ctx, SYM_File_Close, {packs, file});
    CallGuest(ctx, SYM_File_Destroy, {file});
}

void WriteXml(PPCContext& ctx, const std::string& name, uint32_t node) {
    uint32_t str = g_scratch + kScratchString;
    W32(str + kStringSize, 0);
    W32(str + kStringCapacity, kStringInlineCapacity);
    W8(str + kStringText, 0);
    CallGuest(ctx, Method(node, kNodeGetXml), {node, str, 0});
    uint32_t text = str + kStringText;
    if (R32(str + kStringCapacity) > kStringInlineCapacity) text = R32(text);
    WriteLevelFile(ctx, name, text, R32(str + kStringSize));
    CallGuest(ctx, SYM_String_Destroy, {str});
}

// the campaign pack's file into the user map's pack (r4 of 82109AF8); false when the campaign has no such file
bool CopyFile(PPCContext& ctx, const std::string& level, const std::string& name, uint32_t target) {
    uint32_t pack = Text(kScratchPack, "game:\\levels\\" + level + "\\" + level + ".dat");
    uint32_t source = Text(kScratchSource, "levels\\" + level + "\\" + name);
    return CallGuest(ctx, SYM_Pack_CopyFile, {pack, source, Text(kScratchName, name), target}) & 0xFF;
}

void CopyCampaignFiles(PPCContext& ctx, uint32_t target) {
    std::string level = CampaignLevel();
    for (const char* name : kCopiedFiles) CopyFile(ctx, level, name, target);
    CopyFile(ctx, level, level + kDependencies, target);
    uint32_t graph = 0;
    while (graph < kMaxGraphs && CopyFile(ctx, level, "Graph" + std::to_string(graph) + ".bai", target)) graph++;
}

// the map's name as the save page took it, for the script beside the executable
std::string MapName() {
    uint32_t manager = R32(SYM_Editor_Manager_pointer);
    std::string name;
    for (uint32_t i = 0; i < kMapNameChars; i++) {
        uint16_t c = R16(manager + kMapName + i * 2);
        if (!c) break;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '_';
        name += keep ? char(c) : '_';
    }
    return name.empty() ? CampaignLevel() : name;
}

// a new level's script: the campaign's functions are kept and called, so it starts by changing nothing
std::string ScriptTemplate(const std::string& name) {
    return "-- " + name + ": runs after the campaign level's mission script, whose table is Mission.\n"
           "-- A function set here replaces the campaign's; call the one kept to keep its behaviour.\n"
           "\n"
           "local CampaignOnInit = Mission.OnInit\n"
           "\n"
           "function Mission:OnInit()\n"
           "\tif CampaignOnInit then CampaignOnInit(self) end\n"
           "end\n";
}

std::string LoadScript(const std::string& name) {
    fs::path path = app::ExeDir() / "levels" / (name + ".lua");
    std::ifstream in(path, std::ios::binary);
    if (in) return std::string(std::istreambuf_iterator<char>(in), {});
    std::string text = ScriptTemplate(name);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << text;
    LOGR("level editor: %s written", path.string().c_str());
    return text;
}

void WriteScript(PPCContext& ctx) {
    std::string text = LoadScript(MapName());
    uint32_t bytes = uint32_t(text.size());
    if (bytes > g_buffer_bytes) {
        g_buffer = mem::AllocSystem(bytes);
        g_buffer_bytes = g_buffer ? bytes : 0;
    }
    if (!g_buffer) return;
    for (uint32_t i = 0; i < bytes; i++) W8(g_buffer + i, uint8_t(text[i]));
    WriteLevelFile(ctx, kScriptName, g_buffer, bytes);
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_82109AF8);
PPC_EXTERN_FUNC(__imp__sub_82109798);
PPC_EXTERN_FUNC(__imp__sub_82106B00);

// the save copies the template's Brush.lst (r4 the user map's pack); a campaign level copies its own level files
PPC_FUNC(sub_82109AF8) {
    if (!Saving() || !EnsureScratch()) return __imp__sub_82109AF8(ctx, base);
    CopyCampaignFiles(ctx, uint32_t(ctx.r[4]));
}

// the save writes the template map's LevelData.xml; a campaign level writes its own, naming itself
PPC_FUNC(sub_82109798) {
    uint32_t data = KeptLevelData();
    if (!Saving() || !data || !EnsureScratch()) return __imp__sub_82109798(ctx, base);
    uint32_t info = 0;
    for (uint32_t i = 0; i < ChildCount(data) && !info; i++) {
        if (GuestString(ChildAt(data, i) + kNodeTag) == kLevelInfo) info = ChildAt(data, i);
    }
    if (info) {
        uint32_t key = Text(kScratchSource, kCampaignLevelKey);
        CallGuest(ctx, Method(info, kNodeSetAttr), {info, key, Text(kScratchName, CampaignLevel())});
    }
    WriteXml(ctx, kLevelData, data);
}

// the save writes the mission (r3 the mission object); a campaign level writes the campaign's with the editor's
// entities, then its script
PPC_FUNC(sub_82106B00) {
    if (!Saving() || !EnsureScratch()) return __imp__sub_82106B00(ctx, base);
    uint32_t object = uint32_t(ctx.r[3]);
    uint32_t mission = MissionToSave(ctx, object);
    if (!mission) return __imp__sub_82106B00(ctx, base);
    WriteXml(ctx, GuestString(object + kMissionFile), mission);
    CallGuest(ctx, Method(mission, kNodeRelease), {mission});
    WriteScript(ctx);
    LOGR("level editor: %s saved as %s", CampaignLevel().c_str(), MapName().c_str());
}
