// ige.xex, the solo level editor: campaign levels open in the editor. The game's own StartLevel loads the level's pack
// and Mission0 as default.xex does; the editor's setup then runs without what its map templates add (environment
// presets, ambience, cleared sectors, forced water and border heights, rebuilt sector textures and shadows), so the
// level stays as the game shows it. A custom level saved from one, a folder in levels\ beside the executable laid over
// game:\Levels, opens the same way with the campaign level's pack for its assets, its own Mission.lua run after the
// campaign's mission script and again whenever that file changes, and its own copy of the mission's string table, when
// it has one, read in place of the campaign's. CREATE A NEW LEVEL makes a custom level from the editor template picked
// (new_level.cpp) and opens it the same way with the template's pack, saving it once its first frame has read it; its
// Mission.lua is its mission's script, so it is not run a second time. A map keeps its mission too, so it gets the
// same mission objects.
#include "leveleditor/campaign_levels.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/archetype_library.h"
#include "leveleditor/archetypes.h"
#include "leveleditor/event_links.h"
#include "leveleditor/environment.h"
#include "leveleditor/equipment.h"
#include "leveleditor/formations.h"
#include "leveleditor/guest.h"
#include "leveleditor/level_data.h"
#include "leveleditor/markers.h"
#include "leveleditor/mission.h"
#include "leveleditor/mission_text.h"
#include "leveleditor/new_level.h"
#include "leveleditor/sequences.h"
#include "leveleditor/static_geometry.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

namespace fs = std::filesystem;

constexpr const char* kMission = "Mission0";
// the mission loader (8229CC08) runs this after the mission's script and before Mission:OnInit
constexpr const char* kMissionTools = "\\SCRIPTS\\MissionTools.lua";
constexpr const char* kOwnScript = "Mission.lua";
// the script's Language object: its language manager, then the mission table's name (std::string); the manager's first
// field is the language's folder (std::string)
constexpr uint32_t kLanguageManager = 0x0C;
constexpr uint32_t kMissionTable = 0x10;
constexpr const char* kLanguages = "LANGUAGES/";
// ige only knows the editor templates' dependencies; default.xex loads a level's own unless it is an editor map
constexpr const char* kEditorDependencies = "\\Levels\\MET_Dependency\\MET_Dependency_DepLoad.xml";

// the editor's interface manager
constexpr uint32_t kActiveHandler = 0x20;
constexpr uint32_t kCommonBanksLoaded = 0x2C;
constexpr uint32_t kPlayerPlaced = 0x2F;  // the player is put at the tool cursor when this is 0
constexpr uint32_t kStartingLevel = 0x35;
constexpr uint32_t kLevelRequest = 0x38;    // std::string
constexpr uint32_t kLevelRequested = 0x54;  // the next update starts kLevelRequest
constexpr uint32_t kMapName = 0x68;         // UTF-16
constexpr uint32_t kThumbnail = 0x194;      // texture
constexpr uint32_t kEnvironment = 0x1F0;
constexpr uint32_t kTheme = 0x1F4;
constexpr uint32_t kTimeOfDay = 0x1F8;
constexpr uint32_t kFogDistance = 0x200;
constexpr uint32_t kTemplateName = 0x204;  // std::string
constexpr uint32_t kFromTemplate = 0x1EC;
constexpr uint32_t kTemplateDaytime = 1;
constexpr uint32_t kTemplateFogDistance = 9;
constexpr uint32_t kMapManagerHandler = 0x82971408;
constexpr uint32_t kEditorHandlerName = 0x82067344;  // "EditorInterfaceHandler"
// the game: its menus, and the cheats the player update applies (8216F710); the editor sets 2 and 4, god mode
constexpr uint32_t kGameMenus = 0x344;
constexpr uint32_t kMenuSticks = 0x54;  // menus move with the sticks
constexpr uint32_t kGameCheats = 0x410;
constexpr uint32_t kCheatWeapons = 0x2;
constexpr uint32_t kCheatGodMode = 0x4;
constexpr uint32_t kGodModeVariable = 0x8206E344;  // "GodMode"
// set once at start in ige (821175B4): levels then run the game type scripts instead of scripts/main.lua
constexpr uint32_t kEditorGame = 0x3D4;
// user map info, a new one per level started: 0x164 bytes at +0x30, named for the signed in profile
constexpr uint32_t kUserMapInfo = 0x30;
constexpr uint32_t kUserMapInfoBytes = 0x164;
constexpr uint32_t kProfilesSignedIn = 0x08;
constexpr uint32_t kProfileId = 0x270;
// the editor's own sound banks: vehicles and the common ones
constexpr uint32_t kCommonBanks = 9;
// a vector's first and last
constexpr uint32_t kVectorFirst = 0x04;
constexpr uint32_t kVectorLast = 0x08;

// guest scratch for the texts of a start
constexpr uint32_t kScratchLevel = 0x00;
constexpr uint32_t kScratchMission = 0x40;
constexpr uint32_t kScratchPath = 0x60;
constexpr uint32_t kScratchProfile = 0xE0;
constexpr uint32_t kScratchNameBytes = 0x40;
constexpr uint32_t kScratchMissionBytes = 0x20;
constexpr uint32_t kScratchPathBytes = 0x80;
constexpr uint32_t kScratchBytes = 0x100;

// living entity cylinder phys_geometry (0x6E4) surface_idx (0x34); surface 0 is Materials.dat's first
constexpr uint32_t kLivingCylinderSurface = 0x718;
constexpr uint32_t kDefaultSurface = 0;

constexpr uint32_t kMb = 1024 * 1024;
constexpr const char* kNewLevelName = "NEW_LEVEL_%02d";

// a level to open: a campaign level, or a custom level and the level it was made from
struct Request {
    std::string base;
    std::string custom;
};

std::mutex g_lock;
Request g_requested;        // from the menu
std::string g_base_level;   // the level open's base level, empty for a map; under g_lock
std::string g_custom_name;  // the custom level open
Request g_starting;         // editor thread, requested of the editor's update
std::atomic<bool> g_loading{false};
bool g_custom = false;  // editor thread: a custom level is open
uint32_t g_scratch = 0;
// the custom level's own script: the script system it ran in, and its file's time when it last ran
uint32_t g_scripts = 0;
std::filesystem::file_time_type g_script_time;
uint32_t g_script_buffer = 0;
uint32_t g_script_buffer_bytes = 0;
bool g_save_new = false;  // a new level is saved once its first frame has read it
std::set<std::string> g_base_packs;  // the editor templates' packs opened for new levels

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

bool VectorEmpty(uint32_t vector) {
    uint32_t first = R32(vector + kVectorFirst);
    return !first || R32(vector + kVectorLast) == first;
}

uint32_t VectorCount(uint32_t vector) {
    uint32_t first = R32(vector + kVectorFirst);
    return first ? (R32(vector + kVectorLast) - first) / 4 : 0;
}

// a level started gets a fresh user map info
uint32_t ResetMapInfo(PPCContext& ctx) {
    uint32_t maps = R32(SYM_UserMaps_pointer);
    if (R32(maps + kUserMapInfo)) CallGuest(ctx, SYM_UserMapInfo_Free, {maps});
    uint32_t info = uint32_t(CallGuest(ctx, SYM_Memory_New, {kUserMapInfoBytes}));
    if (info) info = uint32_t(CallGuest(ctx, SYM_UserMapInfo_Construct, {info}));
    W32(maps + kUserMapInfo, info);
    return maps;
}

// as for a new map, named after the profile signed in
void NewMapInfo(PPCContext& ctx, uint32_t manager) {
    uint32_t maps = ResetMapInfo(ctx);
    uint32_t profiles = R32(SYM_Profiles_pointer);
    W32(g_scratch + kScratchProfile, R32(R32(profiles + kProfilesSignedIn) + kProfileId));
    CallGuest(ctx, SYM_UserMapInfo_New, {maps, g_scratch + kScratchProfile, manager + kMapName, 1});
}

void EnterEditor(PPCContext& ctx, uint32_t manager, uint32_t game) {
    CallGuest(ctx, SYM_Editor_EnterLevel, {manager});
    W8(manager + kPlayerPlaced, 0);
    CallGuest(ctx, SYM_Editor_ShowHandler, {manager, kEditorHandlerName});
    W8(manager + kCommonBanksLoaded, 1);
    W8(R32(game + kGameMenus) + kMenuSticks, 0);
    W32(game + kGameCheats, R32(game + kGameCheats) | kCheatGodMode);
    uint32_t god = uint32_t(CallGuest(ctx, SYM_Console_FindVariable, {R32(SYM_Console_pointer), kGodModeVariable}));
    if (god) CallGuest(ctx, SYM_Console_SetVariable, {god, 1});
    W32(game + kGameCheats, R32(game + kGameCheats) | kCheatWeapons);
}

// the template fields the tools read; a campaign level matches no template, so environment and theme read "none"
void SetTemplateFields(PPCContext& ctx, uint32_t manager, uint32_t name) {
    W32(manager + kFogDistance, kTemplateFogDistance);
    CallGuest(ctx, SYM_String_Assign, {manager + kTemplateName, name});
    W32(manager + kTimeOfDay, kTemplateDaytime);
    W32(manager + kEnvironment, uint32_t(CallGuest(ctx, SYM_Editor_EnvironmentOf, {manager + kTemplateName})));
    W32(manager + kTheme, uint32_t(CallGuest(ctx, SYM_Editor_ThemeOf, {manager + kTemplateName})));
    uint32_t sound = uint32_t(CallGuest(ctx, SYM_Sound_Get, {}));
    for (uint32_t i = 0; i < kCommonBanks; i++) {
        CallGuest(ctx, SYM_Sound_LoadBank, {sound, R32(SYM_Editor_CommonBanks + i * 4)});
    }
}

// 820F03A0 as an existing map runs it, up to the environment presets; a campaign level has no textureinfo.ige, so its
// paint stays clear, a custom level saved one
void SetUpEditor(PPCContext& ctx, uint32_t manager) {
    CallGuest(ctx, SYM_Thumbnail_CreateCamera, {});
    CallGuest(ctx, SYM_TerrainPaint_SetUp, {});
    CallGuest(ctx, SYM_Catalogue_LoadTemplateObjects, {});
    CallGuest(ctx, SYM_Tripod_LoadBrushes, {});
    CallGuest(ctx, SYM_Zipline_LoadBrushes, {});
    CallGuest(ctx, SYM_EditorObjects_FromEntities, {});
    if (R32(manager + kThumbnail) == R32(SYM_Texture_None)) {
        CallGuest(ctx, SYM_Thumbnail_CreateTexture, {});
    } else {
        CallGuest(ctx, SYM_Thumbnail_Render, {R32(SYM_Thumbnail_Camera)});
        W8(SYM_Thumbnail_Stale, 1);
    }
    if (g_custom) CallGuest(ctx, SYM_TerrainPaint_Load, {});
    if (!VectorEmpty(SYM_Ziplines)) CallGuest(ctx, SYM_Zipline_Load, {});
    if (!VectorEmpty(SYM_MountedWeapons)) CallGuest(ctx, SYM_MountedWeapon_Load, {});
    CallGuest(ctx, SYM_Editor_UpdateTexts, {0});
    W32(SYM_Editor_NextObjectNumber, VectorCount(SYM_EditorObjects));
}

void LogOpened(const char* level) {
    uint32_t entities = 0;
    ForEachEntity([&](uint32_t) { entities++; });
    mem::Stats s = mem::Query();
    uint64_t used = uint64_t(s.total_pages - s.available_pages) * mem::kPage / kMb;
    uint64_t total = uint64_t(s.total_pages) * mem::kPage / kMb;
    LOGR("level editor: %s open, %u entities, %llu of %llu MB guest memory", level, entities,
         (unsigned long long)used, (unsigned long long)total);
}

// the map's name, which a save names its folder after: a custom level's own, empty for a campaign level
void SetMapName(uint32_t manager, const std::string& name) {
    constexpr uint32_t kMapNameChars = 32;
    uint32_t n = uint32_t(std::min<size_t>(name.size(), kMapNameChars - 1));
    for (uint32_t i = 0; i < n; i++) W16(manager + kMapName + i * 2, uint8_t(name[i]));
    W16(manager + kMapName + n * 2, 0);
}

// a custom level made from an editor template reads its assets from the template's pack; the campaign's packs are
// open from the editor's start (campaign_catalogue.cpp), and a pack keeps its path, so each gets its own block
void OpenBasePack(PPCContext& ctx, const std::string& base) {
    if (base.empty() || IsCampaignLevel(base) || !g_base_packs.insert(base).second) return;
    std::string path = "\\levels\\" + base + "\\" + base + ".dat";
    uint32_t guest_path = mem::AllocSystem(uint32_t(path.size() + 1));
    if (!guest_path) return;
    WriteText(guest_path, path, uint32_t(path.size() + 1));
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    if (!CallGuest(ctx, SYM_PackManager_Open, {packs, guest_path, 0, 0, 0, 0})) {
        LOGE("level editor: %s could not be opened", path.c_str());
    }
}

std::string FreeLevelName() {
    std::error_code ec;
    for (int n = 1;; n++) {
        char name[32];
        std::snprintf(name, sizeof(name), kNewLevelName, n);
        if (!fs::exists(app::ExeDir() / "levels" / name, ec)) return name;
    }
}

// CREATE A NEW LEVEL from the template the editor's create page started (manager +0x38): a custom level folder made
// from it, opened and saved like any other; nothing when the template's files are missing
Request CreateNewLevel(PPCContext& ctx, uint32_t manager) {
    std::string base = GuestString(manager + kLevelRequest);
    OpenBasePack(ctx, base);
    std::string name = FreeLevelName();
    auto read = [&](const std::string& path, std::vector<uint8_t>& out) { return ReadPackFile(ctx, path, out); };
    std::vector<LevelFile> files = NewLevelFiles(name, base, read);
    if (files.empty()) {
        LOGE("level editor: %s has no level files to make a new level from", base.c_str());
        return {};
    }
    fs::path folder = app::ExeDir() / "levels" / name;
    std::error_code ec;
    fs::create_directories(folder / "sdat", ec);
    for (const LevelFile& f : files) {
        std::ofstream(folder / f.name, std::ios::binary)
            .write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
    }
    LOGR("level editor: new level %s made from %s", name.c_str(), base.c_str());
    return {base, name};
}

// a campaign level from its pack, or a custom level from its folder with its base level's pack
void StartCampaignLevel(PPCContext& ctx, uint32_t manager, const std::string& level) {
    OpenBasePack(ctx, BaseLevel());
    uint32_t game = R32(SYM_Game_pointer);
    uint32_t name = WriteText(g_scratch + kScratchLevel, level, kScratchNameBytes);
    uint32_t mission = WriteText(g_scratch + kScratchMission, kMission, kScratchMissionBytes);
    CallGuest(ctx, SYM_Editor_ResetLevel, {manager});
    W8(manager + kStartingLevel, 1);
    CallGuest(ctx, SYM_Game_ShowLoading, {game, name, 0, 1});
    SetMapName(manager, g_custom ? level : std::string());
    NewMapInfo(ctx, manager);
    W8(manager + kFromTemplate, 0);
    g_loading = true;
    CallGuest(ctx, SYM_Game_StartLevel, {game, 0, name, mission, 0, 1, 0, 0});
    g_loading = false;
    EnterEditor(ctx, manager, game);
    // a level made from an editor template takes its theme, which picks the template objects and paint textures
    std::string fields = CampaignBase() ? level : BaseLevel();
    SetTemplateFields(ctx, manager, WriteText(g_scratch + kScratchPath, fields, kScratchPathBytes));
    CallGuest(ctx, SYM_Streaming_Wait, {R32(SYM_Streaming_pointer)});
    SetUpEditor(ctx, manager);
    W8(manager + kStartingLevel, 0);
    LogOpened(level.c_str());
}

// the custom level's script runs again once its file changes, so its functions take the new text
void ReloadOwnScript(PPCContext& ctx) {
    if (!g_custom || !g_scripts) return;
    std::filesystem::path path = OwnScriptPath();
    std::error_code ec;
    std::filesystem::file_time_type time = std::filesystem::last_write_time(path, ec);
    if (ec || time == g_script_time) return;
    g_script_time = time;
    std::ifstream in(path, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(in), {});
    uint32_t bytes = uint32_t(text.size());
    if (bytes > g_script_buffer_bytes) {
        g_script_buffer = mem::AllocSystem(bytes);
        g_script_buffer_bytes = g_script_buffer ? bytes : 0;
    }
    if (!g_script_buffer || !bytes) return;
    for (uint32_t i = 0; i < bytes; i++) W8(g_script_buffer + i, uint8_t(text[i]));
    CallGuest(ctx, SYM_Script_ExecuteBuffer, {g_scripts, g_script_buffer, bytes});
    LOGR("level editor: %s run again", path.string().c_str());
}

// the map manager is up: the editor's next update starts the level asked for
void UpdateRequest(PPCContext& ctx, uint32_t manager) {
    if (R32(manager + kActiveHandler) != kMapManagerHandler) return;
    Request request;
    {
        std::lock_guard lock(g_lock);
        request = std::exchange(g_requested, Request{});
    }
    if (request.base.empty() || !EnsureScratch()) return;
    std::string level = request.custom.empty() ? request.base : request.custom;
    uint32_t name = WriteText(g_scratch + kScratchLevel, level, kScratchNameBytes);
    CallGuest(ctx, SYM_String_Assign, {manager + kLevelRequest, name});
    W8(manager + kLevelRequested, 1);
    g_starting = request;
}

// the level folder the game started: the custom level's, or the campaign level's
std::string StartedLevel() { return g_custom ? g_custom_name : BaseLevel(); }

bool MapStarted() { return BaseLevel().empty(); }

// the custom level's Mission.lua ran: the editor runs it again whenever it changes
void NoteOwnScript(uint32_t scripts) {
    g_scripts = scripts;
    std::error_code ec;
    g_script_time = std::filesystem::last_write_time(OwnScriptPath(), ec);
}

}  // namespace

namespace fcip::editor {

void OpenCampaignLevel(int index) {
    if (index < 0 || index >= int(std::size(kCampaignLevels))) return;
    {
        std::lock_guard lock(g_lock);
        g_requested = {kCampaignLevels[index], {}};
    }
    CloseLevelList();
    if (MapOpen()) ExitToMapManager();
}

void OpenCustomLevel(const CustomLevel& level) {
    if (level.base.empty()) return;
    {
        std::lock_guard lock(g_lock);
        g_requested = {level.base, level.name};
    }
    CloseLevelList();
    if (MapOpen()) ExitToMapManager();
}

void ForgetLevelRequest() {
    std::lock_guard lock(g_lock);
    g_requested = {};
}

std::string BaseLevel() {
    std::lock_guard lock(g_lock);
    return g_base_level;
}

bool IsCampaignLevel(const std::string& name) {
    auto same = [](char a, char b) { return std::toupper(uint8_t(a)) == std::toupper(uint8_t(b)); };
    for (std::string level : kCampaignLevels) {
        if (std::equal(name.begin(), name.end(), level.begin(), level.end(), same)) return true;
    }
    return false;
}

bool CampaignBase() { return IsCampaignLevel(BaseLevel()); }

}  // namespace fcip::editor

PPC_EXTERN_FUNC(__imp__sub_82112AD0);
PPC_EXTERN_FUNC(__imp__sub_821124B0);
PPC_EXTERN_FUNC(__imp__sub_8239D908);
PPC_EXTERN_FUNC(__imp__sub_820E0240);
PPC_EXTERN_FUNC(__imp__sub_82138CD8);
PPC_EXTERN_FUNC(__imp__sub_82138E18);
PPC_EXTERN_FUNC(__imp__sub_823243D8);
PPC_EXTERN_FUNC(__imp__sub_820E3C88);
PPC_EXTERN_FUNC(__imp__sub_82449A08);
PPC_EXTERN_FUNC(__imp__sub_8229CC08);
PPC_EXTERN_FUNC(__imp__sub_82106960);
PPC_EXTERN_FUNC(__imp__sub_8222B3C8);
PPC_EXTERN_FUNC(__imp__sub_823A23D8);

// the editor's interface manager (r3) each frame
PPC_FUNC(sub_82112AD0) {
    if (fcip::editor::g_solo) {
        UpdateRequest(ctx, uint32_t(ctx.r[3]));
        UpdateMission(ctx);
        UpdateLevelData(ctx);
        UpdateEnvironment(ctx);
        UpdateEquipment(ctx);
        UpdateFormations(ctx);
        UpdateArchetypeLibrary(ctx);
        UpdateStaticGeometry(ctx);
        UpdateSequences(ctx);
        ReloadOwnScript(ctx);
        if (std::exchange(g_save_new, false)) SaveCustomLevel(ctx, uint32_t(ctx.r[3]));
        if (ShellUp()) UpdateMarkers(ctx);
    }
    __imp__sub_82112AD0(ctx, base);
}

// the editor starts a level (r3 the manager): ours or a map
PPC_FUNC(sub_821124B0) {
    if (!fcip::editor::g_solo || !EnsureScratch()) return __imp__sub_821124B0(ctx, base);
    uint32_t manager = uint32_t(ctx.r[3]);
    ForgetLinks();
    ForgetMarkers();
    ForgetArchetypes();
    ForgetPlayTest();
    ForgetMission(ctx);
    Request start = std::exchange(g_starting, Request{});
    bool made = start.base.empty() && R8(manager + kFromTemplate);
    if (made) start = CreateNewLevel(ctx, manager);
    {
        std::lock_guard lock(g_lock);
        g_base_level = start.base;
    }
    g_custom = !start.custom.empty();
    g_custom_name = start.custom;
    g_scripts = 0;
    if (start.base.empty()) return __imp__sub_821124B0(ctx, base);
    StartCampaignLevel(ctx, manager, StartedLevel());
    g_save_new = made;
}

// a dependency list loads (r3 the manager, r4 its path); a campaign level gets its own on top of the editor's
PPC_FUNC(sub_8239D908) {
    uint32_t manager = uint32_t(ctx.r[3]);
    bool editor_list = g_loading && ReadCString(uint32_t(ctx.r[4])) == kEditorDependencies;
    __imp__sub_8239D908(ctx, base);
    if (!editor_list) return;
    std::string level = StartedLevel();
    std::string path = "\\levels\\" + level + "\\" + level + "_DepLoad.xml";
    CallGuest(ctx, SYM_Dependencies_Load, {manager, WriteText(g_scratch + kScratchPath, path, kScratchPathBytes)});
}

// a level's game scripts (r3 the game): a campaign level gets default.xex's single player ones, scripts/main.lua,
// whose Init loads what the campaign's entities use (helicommon.lua's HC for the Gunship)
PPC_FUNC(sub_82138CD8) {
    if (!g_loading) return __imp__sub_82138CD8(ctx, base);
    uint32_t game = uint32_t(ctx.r[3]);
    uint8_t editor = R8(game + kEditorGame);
    W8(game + kEditorGame, 0);
    __imp__sub_82138CD8(ctx, base);
    W8(game + kEditorGame, editor);
}

// a level's mission objects into the AI system (r5 the mission's XmlNodeRef); the mission is kept, a campaign level's
// as it loads, a map's (its template's, or its own once saved) the first time
PPC_FUNC(sub_8229CC08) {
    bool map = fcip::editor::g_solo && MapStarted() && !KeptMission();
    if (g_loading || map) KeepMission(ctx, R32(uint32_t(ctx.r[5])));
    __imp__sub_8229CC08(ctx, base);
}

// a map's save adds the EntityLibrary to its mission (r4 the mission's XmlNodeRef, which it releases), the last step
// before the file is written; the kept mission's AI objects and the sections the editor leaves out go in after it
PPC_FUNC(sub_82106960) {
    uint32_t mission = R32(uint32_t(ctx.r[4]));
    __imp__sub_82106960(ctx, base);
    if (fcip::editor::g_solo && MapStarted()) AddKeptMission(ctx, mission);
}

// a map template's time of day preset; a campaign level keeps its mission's environment
PPC_FUNC(sub_820E0240) {
    if (fcip::editor::g_solo && !MapStarted()) return;
    __imp__sub_820E0240(ctx, base);
}

// snaps an editor object to its entity: r3 null for a campaign entity with no editor object (AI, triggers)
PPC_FUNC(sub_820E3C88) {
    if (uint32_t(ctx.r[3])) __imp__sub_820E3C88(ctx, base);
}

// a living entity (r3 back): its cylinder's surface is left unset, and the editor's ground ray reads it
PPC_FUNC(sub_82449A08) {
    __imp__sub_82449A08(ctx, base);
    W32(uint32_t(ctx.r[3]) + kLivingCylinderSurface, kDefaultSurface);
}

// a script file runs (r3 the script system, r4 its path); a custom level's own script runs after MissionTools.lua,
// unless it is the mission's script, as a new level's is
PPC_FUNC(sub_823243D8) {
    std::string path = ReadCString(uint32_t(ctx.r[4]));
    bool custom = g_loading && g_custom;
    bool tools = custom && path == kMissionTools;
    bool own = custom && fcip::IsOwnScript(g_custom_name, path);
    uint32_t scripts = uint32_t(ctx.r[3]);
    __imp__sub_823243D8(ctx, base);
    if (own) NoteOwnScript(scripts);
    if (!tools || g_scripts) return;
    uint64_t result = ctx.r[3];
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t name = WriteText(g_scratch + kScratchPath, kOwnScript, kScratchPathBytes);
    uint32_t own_path = uint32_t(CallGuest(ctx, SYM_Engine_LevelFilePath, {engine, name, 0}));
    CallGuest(ctx, SYM_Script_ExecuteFile, {scripts, own_path, 0, 0});
    NoteOwnScript(scripts);
    ctx.r[3] = result;
}

// a level's LevelData.xml loads (r3 its XmlNodeRef out); a campaign level's is kept for the save
PPC_FUNC(sub_82138E18) {
    uint32_t out = uint32_t(ctx.r[3]);
    __imp__sub_82138E18(ctx, base);
    if (g_loading) KeepLevelData(ctx, R32(out));
}

// Language:LoadMissionStringTable (r3 the script's Language object): a custom level's own copy loads under its own
// name, which the level's Localize calls then read; the table is kept for the Objectives tab
PPC_FUNC(sub_8222B3C8) {
    uint32_t language = uint32_t(ctx.r[3]);
    __imp__sub_8222B3C8(ctx, base);
    if (!g_loading || !EnsureScratch()) return;
    uint64_t result = ctx.r[3];
    std::string name = GuestString(language + kMissionTable);
    uint32_t manager = R32(language + kLanguageManager);
    std::filesystem::path own = g_custom ? fcip::OwnStringTable(g_custom_name, name) : std::filesystem::path();
    std::string text;
    if (!own.empty()) {
        std::string own_name = fcip::OwnStringTableName(g_custom_name, name);
        uint32_t text_at = WriteText(g_scratch + kScratchPath, own_name, kScratchPathBytes);
        CallGuest(ctx, SYM_String_Assign, {language + kMissionTable, text_at});
        CallGuest(ctx, SYM_Language_LoadTable, {manager, language + kMissionTable});
        std::ifstream in(own, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    } else {
        std::vector<uint8_t> bytes;
        ReadPackFile(ctx, kLanguages + GuestString(manager) + "/" + name, bytes);
        text.assign(bytes.begin(), bytes.end());
    }
    KeepStringTable(name, text, !own.empty());
    ctx.r[3] = result;
}

// an XML file loads (r3 the file, r4 its path): the custom level's own table name opens its copy
PPC_FUNC(sub_823A23D8) {
    std::string path = ReadCString(uint32_t(ctx.r[4]));
    if (g_custom && path.starts_with(kLanguages) && EnsureScratch()) {
        std::string own = fcip::OwnStringTablePath(g_custom_name, path);
        if (!own.empty()) ctx.r[4] = WriteText(g_scratch + kScratchPath, own, kScratchPathBytes);
    }
    __imp__sub_823A23D8(ctx, base);
}
