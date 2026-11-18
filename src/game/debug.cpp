// default.xex's debug page. Invincibility keeps script writes from lowering the player's health. A level starts at
// once from the menus; in a level it goes through the game's own level change, which relaunches the title with
// "-map <level>" and the story's state. A level starting without a story gets one as PLAY A GAME WITHOUT SAVING makes,
// else it crashes on its first save.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include "common/custom_levels.h"
#include "common/input.h"
#include "common/menu.h"
#include "common/settings.h"
#include "game/custom_campaign.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kPlayerHealth = 0xE8;  // CPlayer, the script's cnt.health
constexpr uint32_t kStorySlot = 0x30;   // save manager: the running story, null outside one
constexpr uint32_t kProfile = 8;        // user manager: the player's profile
constexpr uint32_t kProfileActive = 0x254;  // set once START picks a pad
constexpr uint32_t kCommandBytes = 96;
// NewStory's campaign, Evolution for 2x and 3x levels, else Instincts, and its no saving flag
constexpr uint32_t kInstincts = 0;
constexpr uint32_t kEvolution = 1;
constexpr uint32_t kNoSaving = 1;

std::atomic<const char*> g_level{nullptr};

// a player has pressed START at the title screen
bool SignedIn() {
    uint32_t users = R32(SYM_UserManager_pointer);
    uint32_t profile = users ? R32(users + kProfile) : 0;
    return profile && R8(profile + kProfileActive);
}

bool QueueLevel(const char* folder) {
    if (!R32(SYM_SaveManager_pointer)) return false;
    g_level = folder;
    return true;
}

const bool g_registered = (fcip::g_load_level = QueueLevel, true);

constexpr std::string_view kStartLevel = "StartLevel level=";

// a level has started in this process; leaving one relaunches the title
bool g_in_level = false;
bool g_instant = false;  // the level change skips the loading screen's two second minimum

// a custom level's campaign level, empty for a campaign level
std::string CampaignOf(const std::string& level) {
    for (const fcip::CustomLevel& c : fcip::FindCustomLevels()) {
        if (c.name == level) return c.base;
    }
    return {};
}

// in a level the game's level change, which relaunches the title; from the menus the level starts at once
std::string LevelCommand(const char* folder) {
    g_instant = g_in_level;
    if (g_in_level) return std::string("FadeOutRestartStartLevel level=") + folder;
    return std::string(kStartLevel) + folder + " mission=" + folder + " checkpoint=";
}

bool g_issuing = false;  // the command queued is the debug page's own

void IssueLevel(PPCContext& ctx, uint32_t game) {
    if (!g_level) return;
    static const uint32_t command = mem::AllocSystem(kCommandBytes);
    // at the title screen START signs the player in first, pressed again each second until it takes
    if (!SignedIn()) {
        auto now = std::chrono::steady_clock::now();
        static auto next = now;
        if (now < next) return;
        next = now + std::chrono::seconds(1);
        fcip::PressStart();
        return;
    }
    std::string text = LevelCommand(g_level.exchange(nullptr));
    std::memcpy(HostPtr(command), text.c_str(), text.size() + 1);
    g_issuing = true;
    CallGuest(ctx, SYM_Game_Command, {game, command});
    g_issuing = false;
}

// a story as PLAY A GAME WITHOUT SAVING makes one, before a level starts without one
void NoteLevelStart(PPCContext& ctx, const std::string& command) {
    if (!command.starts_with(kStartLevel)) return;
    g_in_level = true;
    uint32_t manager = R32(SYM_SaveManager_pointer);
    if (!manager || R32(manager + kStorySlot)) return;
    std::string level = command.substr(kStartLevel.size(), command.find(' ', kStartLevel.size()) - kStartLevel.size());
    std::string campaign_level = CampaignOf(level);
    if (campaign_level.empty()) campaign_level = level;
    uint32_t campaign = fcip::EvolutionLevel(campaign_level) ? kEvolution : kInstincts;
    CallGuest(ctx, SYM_SaveManager_NewStory, {manager, campaign, kNoSaving});
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Game_Command));
PPC_EXTERN_FUNC(GUEST_ORIG(LoadingScreen_Wait));
PPC_EXTERN_FUNC(GUEST_ORIG(Game_Update));
PPC_EXTERN_FUNC(GUEST_ORIG(Player_SetProperty));

// (game, command text): queues a command for the next update
PPC_FUNC(GUEST(Game_Command)) {
    std::string routed = fcip::RouteLevelCommand(ReadCString(uint32_t(ctx.r[4])), g_issuing);
    if (!routed.empty()) {
        static const uint32_t text = mem::AllocSystem(kCommandBytes);
        size_t n = std::min<size_t>(routed.size(), kCommandBytes - 1);
        std::memcpy(HostPtr(text), routed.c_str(), n);
        W8(text + uint32_t(n), 0);
        ctx.r[4] = text;
    }
    NoteLevelStart(ctx, ReadCString(uint32_t(ctx.r[4])));
    GUEST_ORIG(Game_Command)(ctx, base);
}

// draws the loading screen until two seconds after it was raised
PPC_FUNC(GUEST(LoadingScreen_Wait)) {
    if (!g_instant) GUEST_ORIG(LoadingScreen_Wait)(ctx, base);
}

// once a frame on the main thread, before the game runs its queued commands; r3 the game
PPC_FUNC(GUEST(Game_Update)) {
    IssueLevel(ctx, uint32_t(ctx.r[3]));
    GUEST_ORIG(Game_Update)(ctx, base);
}

// a script write to a player's cnt property; every health loss goes through here
PPC_FUNC(GUEST(Player_SetProperty)) {
    uint32_t player = fcip::LocalPlayer();
    uint32_t before = player ? R32(player + kPlayerHealth) : 0;
    GUEST_ORIG(Player_SetProperty)(ctx, base);
    if (!player || !fcip::Get().invincible) return;
    bool lowered = GF(player + kPlayerHealth) < std::bit_cast<float>(before);
    if (lowered) W32(player + kPlayerHealth, before);
}
