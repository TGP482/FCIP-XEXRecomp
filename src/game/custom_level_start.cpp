// default.xex: custom levels start as campaign levels do. A custom level is a folder in levels\ beside the exe laid
// over game:\Levels, with loose files and no pack of its own: its base level's pack (a campaign level's or an editor
// template's) opens in its place for the assets, and its Mission.lua runs after MissionTools.lua as it does in the
// level editor, unless it is the mission's own script, as a level made from an editor template's is; its own copy of
// the mission's string table, when it has one, is read in place of the campaign's. CUSTOM CAMPAIGN plays them in
// folder name order: the story mode start goes to the first, and a level's end goes to the next.
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_levels.h"
#include "core/log.h"
#include "game/custom_campaign.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr const char* kMissionTools = "\\SCRIPTS\\MissionTools.lua";
constexpr const char* kCustomMission = "Mission0";  // the mission the level editor saves
constexpr const char* kLastLevelEnd = "FadeOutEndMapReboot";
constexpr uint32_t kTextBytes = 0x80;
// the script's Language object: its language manager, then the mission table's name (std::string)
constexpr uint32_t kLanguageManager = 0x0C;
constexpr uint32_t kMissionTable = 0x10;

// guest main thread
std::string g_running;  // the custom level loading or running, empty for a campaign level
bool g_own_ran = false;  // its Mission.lua ran as its mission's script, as a level made from an editor template's does
uint32_t g_text = 0;
// CUSTOM CAMPAIGN's first level, until the story mode start takes it; set on the menu thread
std::mutex g_start_lock;
std::string g_start;

std::string TakeStart() {
    std::lock_guard lock(g_start_lock);
    return std::exchange(g_start, {});
}

uint32_t GuestText(const std::string& text) {
    if (!g_text) g_text = mem::AllocSystem(kTextBytes);
    size_t n = std::min<size_t>(text.size(), kTextBytes - 1);
    for (size_t i = 0; i < n; i++) W8(g_text + uint32_t(i), uint8_t(text[i]));
    W8(g_text + uint32_t(n), 0);
    return g_text;
}

// a std::string: its text inline until the capacity reaches 16 bytes, else a pointer
std::string GuestString(uint32_t str) {
    constexpr uint32_t kText = 0x04;
    constexpr uint32_t kCapacity = 0x18;
    constexpr uint32_t kInlineBytes = 0x10;
    uint32_t text = str + kText;
    if (R32(str + kCapacity) >= kInlineBytes) text = R32(text);
    return ReadCString(text);
}

std::string CampaignOf(const std::string& level) {
    for (const fcip::CustomLevel& c : fcip::FindCustomLevels()) {
        if (c.name == level) return c.base;
    }
    return {};
}

// the custom level after level, empty after the last
std::string NextLevel(const std::string& level) {
    std::vector<fcip::CustomLevel> levels = fcip::FindCustomLevels();
    auto it = std::find_if(levels.begin(), levels.end(), [&](const fcip::CustomLevel& c) { return c.name == level; });
    return it == levels.end() || ++it == levels.end() ? std::string() : it->name;
}

// where a "key=value" word's value starts, npos when absent
size_t ValueAt(const std::string& command, const char* key) {
    size_t at = command.find(key);
    return at == std::string::npos ? at : at + std::char_traits<char>::length(key);
}

std::string Word(const std::string& command, size_t at) { return command.substr(at, command.find(' ', at) - at); }

std::string WithValue(std::string command, const char* key, const std::string& value) {
    size_t at = ValueAt(command, key);
    if (at != std::string::npos) command.replace(at, Word(command, at).size(), value);
    return command;
}

}  // namespace

namespace fcip {

std::string BeginCustomCampaign() {
    std::vector<CustomLevel> levels = FindCustomLevels();
    if (levels.empty()) return {};
    std::lock_guard lock(g_start_lock);
    g_start = levels.front().name;
    LOGR("custom campaign: starting at %s", g_start.c_str());
    return levels.front().base;
}

void CancelCustomCampaign() { TakeStart(); }

// Level starts: "<prefix>StartLevel level=X mission=Y ...", or a script's "StartLevel X". A level change in a level
// (FadeOut, Restart or a script's) that leaves a custom level for a campaign level, as its end trigger's NextMap
// does, goes to the next custom level instead; a custom level's mission is always Mission0.
std::string RouteLevelCommand(const std::string& command, bool ours) {
    size_t start = command.find("StartLevel");
    if (start == std::string::npos) return {};
    size_t at = ValueAt(command, "level=");
    bool keyed = at != std::string::npos;
    if (!keyed) at = start + std::char_traits<char>::length("StartLevel ");
    if (at >= command.size()) return {};
    std::string level = Word(command, at);
    std::string target = level;
    bool continuing = command.find("checkpoint=Yes") != std::string::npos;  // a saved game's own level
    bool in_level = !keyed || command.starts_with("FadeOut") || command.starts_with("Restart");
    // the next level start takes it, whichever it is
    std::string first = TakeStart();
    if (!first.empty() && !continuing && !in_level) {
        target = first;
    } else if (in_level && !ours && !g_running.empty() && CampaignOf(level).empty()) {
        target = NextLevel(g_running);
        if (target.empty()) {
            LOGR("custom campaign: %s was the last level", g_running.c_str());
            return kLastLevelEnd;
        }
    }
    if (CampaignOf(target).empty()) return {};
    std::string routed = keyed ? WithValue(command, "level=", target) : "FadeOutRestartStartLevel level=" + target;
    routed = WithValue(routed, "mission=", kCustomMission);
    if (routed == command) return {};
    LOGR("custom levels: \"%s\" queued as \"%s\"", command.c_str(), routed.c_str());
    return routed;
}

}  // namespace fcip

PPC_EXTERN_FUNC(GUEST_ORIG(Streaming_OpenLevelPack));
PPC_EXTERN_FUNC(GUEST_ORIG(Script_ExecuteFile));
PPC_EXTERN_FUNC(GUEST_ORIG(Language_LoadMissionStringTable));
PPC_EXTERN_FUNC(GUEST_ORIG(XmlFile_Load));

// a level's pack opens (r4 its folder, r5 1 for a user map); a custom level's campaign level's opens in its place
PPC_FUNC(GUEST(Streaming_OpenLevelPack)) {
    std::string level = ReadCString(uint32_t(ctx.r[4]));
    std::string campaign = uint32_t(ctx.r[5]) ? std::string() : CampaignOf(level);
    g_running = campaign.empty() ? std::string() : level;
    g_own_ran = false;
    if (!campaign.empty()) ctx.r[4] = GuestText(campaign);
    GUEST_ORIG(Streaming_OpenLevelPack)(ctx, base);
}

// a script file runs (r3 the script system, r4 its path); a custom level's own script runs after MissionTools.lua,
// unless it ran as the mission's script
PPC_FUNC(GUEST(Script_ExecuteFile)) {
    std::string path = ReadCString(uint32_t(ctx.r[4]));
    bool tools = !g_running.empty() && path == kMissionTools;
    if (fcip::IsOwnScript(g_running, path)) g_own_ran = true;
    uint32_t scripts = uint32_t(ctx.r[3]);
    GUEST_ORIG(Script_ExecuteFile)(ctx, base);
    if (!tools || g_own_ran) return;
    uint64_t result = ctx.r[3];
    ctx.r[3] = scripts;
    ctx.r[4] = GuestText("\\Levels\\" + g_running + "\\Mission.lua");
    ctx.r[5] = 0;
    ctx.r[6] = 0;
    GUEST_ORIG(Script_ExecuteFile)(ctx, base);
    ctx.r[3] = result;
}

// Language:LoadMissionStringTable (r3 the script's Language object): a custom level's own copy of the table loads
// under its own name, which the level's Localize calls then read
PPC_FUNC(GUEST(Language_LoadMissionStringTable)) {
    uint32_t language = uint32_t(ctx.r[3]);
    GUEST_ORIG(Language_LoadMissionStringTable)(ctx, base);
    std::string name = GuestString(language + kMissionTable);
    if (g_running.empty() || fcip::OwnStringTable(g_running, name).empty()) return;
    uint64_t result = ctx.r[3];
    std::string own = fcip::OwnStringTableName(g_running, name);
    CallGuest(ctx, SYM_String_AssignText, {language + kMissionTable, GuestText(own), uint32_t(own.size())});
    CallGuest(ctx, SYM_Language_LoadTable, {R32(language + kLanguageManager), language + kMissionTable});
    ctx.r[3] = result;
    LOGR("custom levels: %s's own string table loaded", g_running.c_str());
}

// an XML file loads (r3 the file, r4 its path): the custom level's own table name opens its copy
PPC_FUNC(GUEST(XmlFile_Load)) {
    std::string own = fcip::OwnStringTablePath(g_running, ReadCString(uint32_t(ctx.r[4])));
    if (!own.empty()) ctx.r[4] = GuestText(own);
    GUEST_ORIG(XmlFile_Load)(ctx, base);
}
