// ige.xex, the solo level editor: the campaign's types in the Add Object tool. Enemies take the multiplayer items'
// place, objects (props, destroyables, doors) follow the vehicles, solo pickups and logic follow the weapons and
// items. Each campaign mission defines the types it uses in its EntityLibrary and its pack holds their assets, so the
// packs stay open and the types join the editor's archetype library.
#include <algorithm>
#include <string>
#include <vector>

#include "common/widgets.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

// both campaigns, by level folder
constexpr const char* kCampaignLevels[] = {
    "00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs",
    "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest", "21_dunea", "22_duneb",
    "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain",
};
// the AI section of Scripts\Default\EntityClassRegistry.lua
constexpr const char* kEnemyClasses[] = {"Merc", "Worker", "Sapper", "Chucker", "Freak", "Crowe", "Dragon", "Valerie"};
constexpr const char* kObjectClasses[] = {"DestroyableObject", "BasicEntity", "AutomaticDoor"};

// a class the catalogue spawns with its script's defaults
struct ClassEntry {
    const char* cls;
    const char* caption;
};
// the campaign's pickups the multiplayer list lacks; their scripts carry the model
constexpr ClassEntry kSoloPickups[] = {
    {"PickupAdrenaline", "Adrenaline"},
    {"MRE", "Ration Pack"},
    {"PickupGrenadeBox", "Grenade Box"},
    {"PickupMolotovBox", "Molotov Box"},
    {"PickupBerrettaNoSup", "9mm Pistol (No Silencer)"},
    {"PickupBinoculars", "Binoculars"},
    {"PickupExplo", "Explosives"},
    {"PickupMortar", "Mortar"},
    {"PickupBrowning", "Heavy Machine Gun"},
    {"Secret", "Secret"},
};
// the logic the campaign scripts its levels with; none has a model, so the editor marks them
constexpr ClassEntry kLogicObjects[] = {
    {"Checkpoint", "Checkpoint"},
    {"EndMapTrigger", "Level End"},
    {"ProximityTrigger", "Proximity Trigger"},
    {"AreaTrigger", "Area Trigger"},
    {"DelayTrigger", "Delay Trigger"},
    {"MultipleTrigger", "Multiple Trigger"},
    {"UtilityTrigger", "Utility Trigger"},
    {"Waypoint", "Waypoint"},
};

// category captions by the editor's ObjectCategoryNames key
struct CategoryCaption {
    const char* key;
    const char* caption;
};
constexpr CategoryCaption kCategoryCaptions[] = {
    {"Vehicles", "{DPLR} VEHICLES AND OBJECTS (1/8)"},
    {"Weapons", "{DPLR} ITEMS AND LOGIC (2/8)"},
    {"MultiplayerItems", "{DPLR} ENEMIES (3/8)"},
};
// the multiplayer list's keys end in a two letter team suffix, which its tool strips before spawning
constexpr const char* kTeamSuffix = "_E";

// std::string and std::wstring
constexpr uint32_t kStrText = 0x04;
constexpr uint32_t kStrLength = 0x14;
constexpr uint32_t kStrCapacity = 0x18;
constexpr uint32_t kStrEmptyCapacity = 0x0F;
constexpr uint32_t kWStrEmptyCapacity = 0x07;

// a catalogue entry: the key the class and archetype are found by, its caption (std::wstring), the archetype
constexpr uint32_t kEntryKey = 0x00;
constexpr uint32_t kEntryCaption = 0x1C;
constexpr uint32_t kEntryArchetype = 0x38;
constexpr uint32_t kEntryBytes = 0x54;
// the catalogue's lists, one per section of the editor's string table
constexpr uint32_t kListFirst = 0x04;
constexpr uint32_t kListLast = 0x08;
constexpr uint32_t kListStride = 0x14;
constexpr uint32_t kVehicleList = 0;
constexpr uint32_t kWeaponList = 1;
constexpr uint32_t kMultiplayerList = 2;

// guest scratch for the calls below
constexpr uint32_t kScratchRef = 0x00;
constexpr uint32_t kScratchChildRef = 0x04;
constexpr uint32_t kScratchPassRef = 0x08;
constexpr uint32_t kScratchString = 0x10;
constexpr uint32_t kScratchEntry = 0x30;
constexpr uint32_t kScratchText = 0x90;
constexpr uint32_t kScratchTextBytes = 0x100;
constexpr uint32_t kScratchBytes = kScratchText + kScratchTextBytes;

struct CampaignType {
    std::string archetype;  // the catalogue key, an enemy's less the team suffix
    std::string cls;
    std::string caption;
};

uint32_t g_scratch = 0;
bool g_packs_open = false;
std::vector<CampaignType> g_enemies;
std::vector<CampaignType> g_objects;

uint32_t GuestText(const std::string& text) {
    return WriteText(g_scratch + kScratchText, text, kScratchTextBytes);
}

void InitEmptyString(uint32_t str, uint32_t capacity) {
    W32(str + kStrText, 0);
    W32(str + kStrLength, 0);
    W32(str + kStrCapacity, capacity);
}

void Release(PPCContext& ctx, uint32_t node) {
    if (node) CallGuest(ctx, Method(node, kNodeRelease), {node});
}

std::string Attr(PPCContext& ctx, uint32_t node, const char* key) {
    uint32_t value = uint32_t(CallGuest(ctx, Method(node, kNodeGetAttr), {node, GuestText(key)}));
    return value ? ReadCString(value) : std::string();
}

template <size_t N>
bool OneOf(const char* const (&classes)[N], const std::string& cls) {
    return std::find(std::begin(classes), std::end(classes), cls) != std::end(classes);
}

const CampaignType* TypeByKey(const std::string& key) {
    for (const std::vector<CampaignType>* types : {&g_enemies, &g_objects}) {
        for (const CampaignType& t : *types) {
            if (t.archetype == key) return &t;
        }
    }
    return nullptr;
}

// "EnemyArchetypesLibrary.Merc1.Scout" reads "Merc1 Scout"
std::string CaptionOf(const std::string& archetype) {
    std::string caption = archetype.substr(archetype.find('.') + 1);
    std::replace(caption.begin(), caption.end(), '.', ' ');
    std::replace(caption.begin(), caption.end(), '_', ' ');
    return caption;
}

void SortByCaption(std::vector<CampaignType>& types) {
    std::sort(types.begin(), types.end(), [](const CampaignType& a, const CampaignType& b) {
        return a.caption < b.caption;
    });
}

// the pack keeps its path, so each gets its own block
void OpenCampaignPacks(PPCContext& ctx) {
    if (g_packs_open) return;
    g_packs_open = true;
    uint32_t packs = uint32_t(CallGuest(ctx, SYM_PackManager_Get, {}));
    for (const char* level : kCampaignLevels) {
        std::string path = std::string("\\levels\\") + level + "\\" + level + ".dat";
        uint32_t guest_path = mem::AllocSystem(uint32_t(path.size() + 1));
        for (size_t i = 0; i <= path.size(); i++) W8(guest_path + uint32_t(i), uint8_t(path.c_str()[i]));
        uint32_t pack = uint32_t(CallGuest(ctx, SYM_PackManager_Open, {packs, guest_path, 0, 0, 0, 0}));
        if (!pack) LOGE("level editor: %s could not be opened", path.c_str());
    }
}

uint32_t LoadXml(PPCContext& ctx, const std::string& path) {
    W32(g_scratch + kScratchRef, 0);
    InitEmptyString(g_scratch + kScratchString, kStrEmptyCapacity);
    CallGuest(ctx, SYM_Xml_LoadFile, {g_scratch + kScratchRef, g_scratch + kScratchString, GuestText(path), 0});
    CallGuest(ctx, SYM_String_Destroy, {g_scratch + kScratchString});
    return R32(g_scratch + kScratchRef);
}

void Adopt(PPCContext& ctx, uint32_t library, uint32_t prototype) {
    std::string cls = Attr(ctx, prototype, "Class");
    std::string name = Attr(ctx, prototype, "Name");
    std::vector<CampaignType>* types = nullptr;
    if (OneOf(kEnemyClasses, cls)) types = &g_enemies;
    if (OneOf(kObjectClasses, cls)) types = &g_objects;
    if (!types || name.empty() || TypeByKey(name)) return;
    CallGuest(ctx, Method(prototype, kNodeAddRef), {prototype});
    W32(g_scratch + kScratchPassRef, prototype);
    CallGuest(ctx, Method(library, kNodeAddChild), {library, g_scratch + kScratchPassRef});
    types->push_back({name, cls, CaptionOf(name)});
}

void AdoptMissionTypes(PPCContext& ctx, uint32_t library, uint32_t mission) {
    W32(g_scratch + kScratchChildRef, 0);
    uint32_t tag = GuestText("EntityLibrary");
    CallGuest(ctx, Method(mission, kNodeFindChild), {g_scratch + kScratchChildRef, mission, tag});
    uint32_t prototypes = R32(g_scratch + kScratchChildRef);
    if (!prototypes) return;
    uint32_t count = uint32_t(CallGuest(ctx, Method(prototypes, kNodeChildCount), {prototypes}));
    for (uint32_t i = 0; i < count; i++) {
        W32(g_scratch + kScratchRef, 0);
        CallGuest(ctx, Method(prototypes, kNodeGetChild), {g_scratch + kScratchRef, prototypes, i});
        uint32_t prototype = R32(g_scratch + kScratchRef);
        if (!prototype) continue;
        Adopt(ctx, library, prototype);
        Release(ctx, prototype);
    }
    Release(ctx, prototypes);
}

void AdoptCampaignTypes(PPCContext& ctx, uint32_t library) {
    g_enemies.clear();
    g_objects.clear();
    for (const char* level : kCampaignLevels) {
        std::string path = std::string("\\levels\\") + level + "\\mission_mission0.xml";
        uint32_t mission = LoadXml(ctx, path);
        if (!mission) {
            LOGE("level editor: %s could not be read", path.c_str());
            continue;
        }
        AdoptMissionTypes(ctx, library, mission);
        Release(ctx, mission);
    }
    SortByCaption(g_enemies);
    SortByCaption(g_objects);
    LOGR("level editor: %zu enemy and %zu object types from %zu campaign levels", g_enemies.size(), g_objects.size(),
         std::size(kCampaignLevels));
}

uint32_t List(uint32_t index) { return SYM_Catalogue_Lists + index * kListStride; }

void AddCatalogueEntry(PPCContext& ctx, uint32_t list, const std::string& key, const std::string& caption,
                       const std::string& archetype) {
    uint32_t entry = g_scratch + kScratchEntry;
    InitEmptyString(entry + kEntryKey, kStrEmptyCapacity);
    InitEmptyString(entry + kEntryCaption, kWStrEmptyCapacity);
    InitEmptyString(entry + kEntryArchetype, kStrEmptyCapacity);
    CallGuest(ctx, SYM_String_Assign, {entry + kEntryKey, GuestText(key)});
    CallGuest(ctx, SYM_WString_Assign, {entry + kEntryCaption, fcip::ui::Text(caption.c_str())});
    CallGuest(ctx, SYM_String_Assign, {entry + kEntryArchetype, GuestText(archetype)});
    CallGuest(ctx, SYM_Catalogue_PushBack, {list, entry});
    CallGuest(ctx, SYM_Catalogue_EntryDestroy, {entry});
}

void ClearList(PPCContext& ctx, uint32_t list) {
    uint32_t first = R32(list + kListFirst);
    if (!first) return;
    for (uint32_t e = first; e < R32(list + kListLast); e += kEntryBytes) {
        CallGuest(ctx, SYM_Catalogue_EntryDestroy, {e});
    }
    W32(list + kListLast, first);
}

// the editor reads its catalogue once, before the library is built
void FillCatalogue(PPCContext& ctx) {
    ClearList(ctx, List(kMultiplayerList));
    for (const CampaignType& t : g_enemies) {
        AddCatalogueEntry(ctx, List(kMultiplayerList), t.archetype + kTeamSuffix, t.caption, t.archetype);
    }
    for (const CampaignType& t : g_objects) {
        AddCatalogueEntry(ctx, List(kVehicleList), t.archetype, t.caption, t.archetype);
    }
    for (const ClassEntry& p : kSoloPickups) AddCatalogueEntry(ctx, List(kWeaponList), p.cls, p.caption, "");
    // not the vehicle list: a class there makes its objects vehicles
    for (const ClassEntry& p : kLogicObjects) AddCatalogueEntry(ctx, List(kWeaponList), p.cls, p.caption, "");
}

const char* CategoryCaptionFor(const std::string& key) {
    for (const CategoryCaption& c : kCategoryCaptions) {
        if (key == c.key) return c.caption;
    }
    return nullptr;
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_821AA1D8);
PPC_EXTERN_FUNC(__imp__sub_820F1200);
PPC_EXTERN_FUNC(__imp__sub_8211B9A8);

// the editor's start builds the archetype library (r3, its root XmlNodeRef at +0) right after reading the catalogue
PPC_FUNC(sub_821AA1D8) {
    if (!fcip::editor::g_solo) return __imp__sub_821AA1D8(ctx, base);
    uint32_t holder = uint32_t(ctx.r[3]);
    __imp__sub_821AA1D8(ctx, base);
    uint32_t library = R32(holder);
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    if (!library || !g_scratch) return;
    OpenCampaignPacks(ctx);
    AdoptCampaignTypes(ctx, library);
    FillCatalogue(ctx);
}

// spawns an editor object: r4 the catalogue key (an enemy's stripped of its suffix), which names the class; ours are
// archetypes, so r4 gets their class
PPC_FUNC(sub_820F1200) {
    if (!fcip::editor::g_solo) return __imp__sub_820F1200(ctx, base);
    if (const CampaignType* t = TypeByKey(ReadCString(uint32_t(ctx.r[4])))) ctx.r[4] = GuestText(t->cls);
    __imp__sub_820F1200(ctx, base);
}

// a string of the editor's table: r3 the std::wstring out, r5 the section, r6 the key
PPC_FUNC(sub_8211B9A8) {
    if (!fcip::editor::g_solo) return __imp__sub_8211B9A8(ctx, base);
    uint32_t out = uint32_t(ctx.r[3]);
    const char* caption = nullptr;
    if (GuestString(uint32_t(ctx.r[5])) == "ObjectCategoryNames") {
        caption = CategoryCaptionFor(GuestString(uint32_t(ctx.r[6])));
    }
    __imp__sub_8211B9A8(ctx, base);
    if (caption) CallGuest(ctx, SYM_WString_Assign, {out, fcip::ui::Text(caption)});
}
