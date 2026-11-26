// ige.xex, the solo level editor: the classes the map editor leaves alone. Its class check (820C4A30, the class name
// and whether for the save) turns down Player, MultipleArrow, BranchWhip, Claymore, MountedWeaponVehicle(Mortar) and
// DynamicLight always, and ZipLine and the mounted weapons for its editor objects too (its own tools own those in a
// map). A campaign level places them as entities of their own, so the solo editor gives the mission's ones editor
// objects as the others get them (8210B4D0: 820E2218 then 8210BA10), and the save writes every one that has an
// editor object, which leaves out the lights and weapons scripts spawn for other entities.
#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_set>

#include "kernel/memory.h"
#include "leveleditor/editor_classes.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/mode.h"
#include "leveleditor/picking.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kKeptClasses[] = {
    "DynamicLight",          "ZipLine",     "MountedWeaponBrowning", "MountedWeaponMortar", "MountedWeaponVehicle",
    "MountedWeaponVehicleMortar", "Claymore", "BranchWhip",          "MultipleArrow",
};
constexpr uint32_t kEntityFlags = 0x2D;
constexpr uint8_t kNoEditorObject = 0x80;

thread_local bool t_allow = false;  // the class check says yes while set

std::string ClassOf(PPCContext& ctx, uint32_t entity) {
    uint32_t cls = uint32_t(CallGuest(ctx, Method(entity, kEntityClassName), {entity}));
    return cls ? ReadCString(cls) : std::string();
}

bool IsKept(const std::string& cls) {
    return std::find(std::begin(kKeptClasses), std::end(kKeptClasses), cls) != std::end(kKeptClasses);
}

void AddEditorObjects(PPCContext& ctx) {
    std::unordered_set<uint32_t> mission = MissionEntityIds();
    ForEachEntity([&](uint32_t entity) {
        if (!mission.count(R32(entity + kEntityId)) || (R8(entity + kEntityFlags) & kNoEditorObject)) return;
        if (!IsKept(ClassOf(ctx, entity)) || EditorObjectFor(ctx, entity)) return;
        uint32_t object = uint32_t(CallGuest(ctx, SYM_EditorObject_MakeGeneric, {entity}));
        if (object) CallGuest(ctx, SYM_EditorObject_Register, {object, 0});
    });
}

}  // namespace

bool fcip::editor::SavesAsEntity(PPCContext& ctx, uint32_t entity) {
    return g_solo && IsKept(ClassOf(ctx, entity)) && EditorObjectFor(ctx, entity) != 0;
}

void fcip::editor::AllowClasses(bool allow) { t_allow = allow; }

PPC_EXTERN_FUNC(__imp__sub_820C4A30);
PPC_EXTERN_FUNC(__imp__sub_8210B4D0);

// whether the map editor takes a class (r3 its name, std::string; r4 1 for the save)
PPC_FUNC(sub_820C4A30) {
    if (!t_allow) return __imp__sub_820C4A30(ctx, base);
    ctx.r[3] = 1;
}

// the editor makes editor objects for the level's entities
PPC_FUNC(sub_8210B4D0) {
    __imp__sub_8210B4D0(ctx, base);
    if (g_solo) AddEditorObjects(ctx);
}
