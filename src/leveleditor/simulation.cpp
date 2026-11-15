// ige.xex, the solo level editor: a level stands still while it is edited, as in a level editor's edit mode. The map
// editor keeps the game running under its tools, so a campaign level's AI walked their paths, its triggers and
// sequences played the story intro and dialogue, and physics settled as the level was edited. The system update
// (82313240, which the game's update calls with pause mode 0) gets pause mode 1 while editing, which skips physics
// (82312D30), the AI (821C8A88) and the entity system (82312EC0); play test runs them as the game does.
// Leaving play test puts the level back as play test found it, as the game's own map reset (GameRulesLib
// ResetMapEntities) does for a round: entities spawned since are removed, the rest go back to where they stood and
// are reset (their scripts' OnReset rearms triggers and sends the AI back to their jobs), and sequences stop.
#include <cstring>
#include <unordered_map>
#include <vector>

#include "editor/tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr uint64_t kPauseSimulation = 1;
// the editor's interface handler: +0x104 its mode, which 820C5548 switches
constexpr uint32_t kHandlerMode = 0x104;
constexpr uint32_t kModeEditing = 1;
constexpr uint32_t kModePlayTest = 2;
// entity: SetPos by vtable
constexpr uint32_t kEntitySetPos = 0x14;  // (entity, Vec3, 0)
constexpr uint32_t kScriptEventReset = 4;
// the entity system's RemoveEntity, as System.RemoveEntity (822374F0) calls it
constexpr uint32_t kSystemRemoveEntity = 0x14;  // (system, id, 0, 0)

struct Placement {
    bool has_pos = false;
    float pos[3] = {};
    float angles[3] = {};
};

// editor thread: every entity as play test found it, by id
std::unordered_map<uint32_t, Placement> g_placements;
uint32_t g_vec = 0;  // guest scratch, one Vec3

bool EnsureVec() {
    if (!g_vec) g_vec = mem::AllocSystem(12);
    return g_vec != 0;
}

void ReadVec(uint32_t at, float out[3]) {
    for (uint32_t i = 0; i < 3; i++) out[i] = GF(at + i * 4);
}

uint32_t WriteVec(const float v[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t bits;
        std::memcpy(&bits, &v[i], sizeof(bits));
        W32(g_vec + i * 4, bits);
    }
    return g_vec;
}

void KeepPlacements(PPCContext& ctx) {
    g_placements.clear();
    ForEachEntity([&](uint32_t entity) {
        Placement p;
        uint32_t pos = uint32_t(CallGuest(ctx, Method(entity, kEntityPosition), {entity, 1}));
        p.has_pos = pos != 0;
        if (p.has_pos) ReadVec(pos, p.pos);
        ReadVec(entity + kEntityAngles, p.angles);
        g_placements[R32(entity + kEntityId)] = p;
    });
}

void RestorePlacements(PPCContext& ctx) {
    std::vector<uint32_t> spawned;
    std::vector<uint32_t> kept;
    ForEachEntity([&](uint32_t entity) {
        if (g_placements.count(R32(entity + kEntityId))) {
            kept.push_back(entity);
        } else {
            spawned.push_back(R32(entity + kEntityId));
        }
    });
    uint32_t system = R32(SYM_EntitySystem_pointer);
    for (uint32_t id : spawned) CallGuest(ctx, Method(system, kSystemRemoveEntity), {system, id, 0, 0});
    for (uint32_t entity : kept) {
        const Placement& p = g_placements[R32(entity + kEntityId)];
        if (p.has_pos) CallGuest(ctx, Method(entity, kEntitySetPos), {entity, WriteVec(p.pos), 0});
        CallGuest(ctx, SYM_Entity_SetAngles, {entity, WriteVec(p.angles), 1, 1, 0});
        CallGuest(ctx, SYM_Entity_Reset, {entity});
        CallGuest(ctx, SYM_Entity_ScriptEvent, {entity, kScriptEventReset, 0, 0});
    }
    if (uint32_t movie = R32(SYM_Movie_pointer)) CallGuest(ctx, SYM_Movie_StopAllSequences, {movie});
    g_placements.clear();
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_82313240);
PPC_EXTERN_FUNC(__imp__sub_820C5548);

// the system update (r3 update flags, r4 pause mode), once a frame
PPC_FUNC(sub_82313240) {
    if (g_solo && ShellUp()) ctx.r[4] = kPauseSimulation;
    __imp__sub_82313240(ctx, base);
}

// the editor switches between editing and play test (r3 the handler, r4 the mode)
PPC_FUNC(sub_820C5548) {
    uint32_t handler = uint32_t(ctx.r[3]);
    uint32_t mode = uint32_t(ctx.r[4]);
    uint32_t was = R32(handler + kHandlerMode);
    __imp__sub_820C5548(ctx, base);
    if (!g_solo || was == mode || !EnsureVec()) return;
    if (mode == kModePlayTest) KeepPlacements(ctx);
    if (mode == kModeEditing && !g_placements.empty()) RestorePlacements(ctx);
}

void fcip::editor::ForgetPlayTest() { g_placements.clear(); }
