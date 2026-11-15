// default.xex: custom levels start as campaign levels do. A custom level is a folder in levels\ beside the exe laid
// over game:\Levels, with loose files and no pack of its own: its campaign level's pack opens in its place for the
// assets, and its Mission.lua runs after MissionTools.lua as it does in the level editor.
#include <algorithm>
#include <string>

#include "common/custom_levels.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr const char* kMissionTools = "\\SCRIPTS\\MissionTools.lua";
constexpr uint32_t kTextBytes = 0x80;

std::string g_running;  // the custom level loading or running, empty for a campaign level
uint32_t g_text = 0;

uint32_t GuestText(const std::string& text) {
    if (!g_text) g_text = mem::AllocSystem(kTextBytes);
    size_t n = std::min<size_t>(text.size(), kTextBytes - 1);
    for (size_t i = 0; i < n; i++) W8(g_text + uint32_t(i), uint8_t(text[i]));
    W8(g_text + uint32_t(n), 0);
    return g_text;
}

std::string CampaignOf(const std::string& level) {
    for (const fcip::CustomLevel& c : fcip::FindCustomLevels()) {
        if (c.name == level) return c.campaign;
    }
    return {};
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Streaming_OpenLevelPack));
PPC_EXTERN_FUNC(GUEST_ORIG(Script_ExecuteFile));

// a level's pack opens (r4 its folder, r5 1 for a user map); a custom level's campaign level's opens in its place
PPC_FUNC(GUEST(Streaming_OpenLevelPack)) {
    std::string level = ReadCString(uint32_t(ctx.r[4]));
    std::string campaign = uint32_t(ctx.r[5]) ? std::string() : CampaignOf(level);
    g_running = campaign.empty() ? std::string() : level;
    if (!campaign.empty()) ctx.r[4] = GuestText(campaign);
    GUEST_ORIG(Streaming_OpenLevelPack)(ctx, base);
}

// a script file runs (r3 the script system, r4 its path); a custom level's own script runs after MissionTools.lua
PPC_FUNC(GUEST(Script_ExecuteFile)) {
    bool tools = !g_running.empty() && ReadCString(uint32_t(ctx.r[4])) == kMissionTools;
    uint32_t scripts = uint32_t(ctx.r[3]);
    GUEST_ORIG(Script_ExecuteFile)(ctx, base);
    if (!tools) return;
    uint64_t result = ctx.r[3];
    ctx.r[3] = scripts;
    ctx.r[4] = GuestText("\\Levels\\" + g_running + "\\Mission.lua");
    ctx.r[5] = 0;
    ctx.r[6] = 0;
    GUEST_ORIG(Script_ExecuteFile)(ctx, base);
    ctx.r[3] = result;
}
