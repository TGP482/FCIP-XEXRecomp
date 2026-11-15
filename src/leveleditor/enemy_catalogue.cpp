// ige.xex, the solo level editor: the campaign's enemy types take the multiplayer items' place in the Add Object
// tool. Each campaign mission defines the types it uses in its EntityLibrary and its pack holds their assets, so the
// packs stay open and the types join the editor's archetype library.
#include <algorithm>
#include <string>
#include <vector>

#include "common/widgets.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// both campaigns, by level folder
constexpr const char* kCampaignLevels[] = {
    "00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs",
    "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest", "21_dunea", "22_duneb",
    "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain",
};
// the AI section of Scripts\Default\EntityClassRegistry.lua
constexpr const char* kEnemyClasses[] = {"Merc", "Worker", "Sapper", "Chucker", "Freak", "Crowe", "Dragon", "Valerie"};
constexpr const char* kEnemiesCaption = "{DPLR} ENEMIES (3/8)";

// IXmlNode methods
constexpr uint32_t kNodeAddRef = 0x08;
constexpr uint32_t kNodeRelease = 0x0C;
constexpr uint32_t kNodeGetAttr = 0x4C;
constexpr uint32_t kNodeAddChild = 0x58;  // takes an XmlNodeRef by value, which it releases
constexpr uint32_t kNodeChildCount = 0x68;
constexpr uint32_t kNodeGetChild = 0x6C;
constexpr uint32_t kNodeFindChild = 0x70;

// std::string and std::wstring: inline text until the capacity reaches 16 bytes, else a pointer
constexpr uint32_t kStrText = 0x04;
constexpr uint32_t kStrLength = 0x14;
constexpr uint32_t kStrCapacity = 0x18;
constexpr uint32_t kStrInlineBytes = 0x10;
constexpr uint32_t kStrEmptyCapacity = 0x0F;
constexpr uint32_t kWStrEmptyCapacity = 0x07;

// a catalogue entry: the key the class and archetype are found by, its caption (std::wstring), the archetype
constexpr uint32_t kEntryKey = 0x00;
constexpr uint32_t kEntryCaption = 0x1C;
constexpr uint32_t kEntryArchetype = 0x38;
constexpr uint32_t kEntryBytes = 0x54;
// the catalogue's lists, one per section of the editor's string table; the third is the multiplayer items
constexpr uint32_t kListFirst = 0x04;
constexpr uint32_t kListLast = 0x08;
constexpr uint32_t kListStride = 0x14;
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

struct EnemyType {
    std::string archetype;  // the catalogue key too
    std::string cls;
    std::string caption;
};

uint32_t g_scratch = 0;
bool g_packs_open = false;
std::vector<EnemyType> g_enemies;

uint32_t GuestText(const std::string& text) {
    size_t n = std::min<size_t>(text.size(), kScratchTextBytes - 1);
    for (size_t i = 0; i < n; i++) W8(g_scratch + kScratchText + uint32_t(i), uint8_t(text[i]));
    W8(g_scratch + kScratchText + uint32_t(n), 0);
    return g_scratch + kScratchText;
}

std::string GuestString(uint32_t str) {
    uint32_t text = str + kStrText;
    if (R32(str + kStrCapacity) >= kStrInlineBytes) text = R32(text);
    return ReadCString(text);
}

void InitEmptyString(uint32_t str, uint32_t capacity) {
    W32(str + kStrText, 0);
    W32(str + kStrLength, 0);
    W32(str + kStrCapacity, capacity);
}

uint32_t Method(uint32_t node, uint32_t slot) { return R32(R32(node) + slot); }

void Release(PPCContext& ctx, uint32_t node) {
    if (node) CallGuest(ctx, Method(node, kNodeRelease), {node});
}

std::string Attr(PPCContext& ctx, uint32_t node, const char* key) {
    uint32_t value = uint32_t(CallGuest(ctx, Method(node, kNodeGetAttr), {node, GuestText(key)}));
    return value ? ReadCString(value) : std::string();
}

bool IsEnemyClass(const std::string& cls) {
    return std::find(std::begin(kEnemyClasses), std::end(kEnemyClasses), cls) != std::end(kEnemyClasses);
}

const EnemyType* EnemyByKey(const std::string& key) {
    for (const EnemyType& e : g_enemies) {
        if (e.archetype == key) return &e;
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

void AdoptIfEnemy(PPCContext& ctx, uint32_t library, uint32_t prototype) {
    std::string cls = Attr(ctx, prototype, "Class");
    std::string name = Attr(ctx, prototype, "Name");
    if (!IsEnemyClass(cls) || name.empty() || EnemyByKey(name)) return;
    CallGuest(ctx, Method(prototype, kNodeAddRef), {prototype});
    W32(g_scratch + kScratchPassRef, prototype);
    CallGuest(ctx, Method(library, kNodeAddChild), {library, g_scratch + kScratchPassRef});
    g_enemies.push_back({name, cls, CaptionOf(name)});
}

void AdoptMissionEnemies(PPCContext& ctx, uint32_t library, uint32_t mission) {
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
        AdoptIfEnemy(ctx, library, prototype);
        Release(ctx, prototype);
    }
    Release(ctx, prototypes);
}

void AdoptCampaignEnemies(PPCContext& ctx, uint32_t library) {
    g_enemies.clear();
    for (const char* level : kCampaignLevels) {
        std::string path = std::string("\\levels\\") + level + "\\mission_mission0.xml";
        uint32_t mission = LoadXml(ctx, path);
        if (!mission) {
            LOGE("level editor: %s could not be read", path.c_str());
            continue;
        }
        AdoptMissionEnemies(ctx, library, mission);
        Release(ctx, mission);
    }
    std::sort(g_enemies.begin(), g_enemies.end(), [](const EnemyType& a, const EnemyType& b) {
        return a.caption < b.caption;
    });
    LOGR("level editor: %zu enemy types from %zu campaign levels", g_enemies.size(), std::size(kCampaignLevels));
}

void AddCatalogueEntry(PPCContext& ctx, uint32_t list, const EnemyType& e) {
    uint32_t entry = g_scratch + kScratchEntry;
    InitEmptyString(entry + kEntryKey, kStrEmptyCapacity);
    InitEmptyString(entry + kEntryCaption, kWStrEmptyCapacity);
    InitEmptyString(entry + kEntryArchetype, kStrEmptyCapacity);
    CallGuest(ctx, SYM_String_Assign, {entry + kEntryKey, GuestText(e.archetype)});
    CallGuest(ctx, SYM_WString_Assign, {entry + kEntryCaption, fcip::ui::Text(e.caption.c_str())});
    CallGuest(ctx, SYM_String_Assign, {entry + kEntryArchetype, GuestText(e.archetype)});
    CallGuest(ctx, SYM_Catalogue_PushBack, {list, entry});
    CallGuest(ctx, SYM_Catalogue_EntryDestroy, {entry});
}

void ReplaceMultiplayerItems(PPCContext& ctx) {
    uint32_t list = SYM_Catalogue_Lists + kMultiplayerList * kListStride;
    uint32_t first = R32(list + kListFirst);
    if (first) {
        for (uint32_t e = first; e < R32(list + kListLast); e += kEntryBytes) {
            CallGuest(ctx, SYM_Catalogue_EntryDestroy, {e});
        }
        W32(list + kListLast, first);
    }
    for (const EnemyType& e : g_enemies) AddCatalogueEntry(ctx, list, e);
}

}  // namespace

PPC_EXTERN_FUNC(__imp__sub_821AA1D8);
PPC_EXTERN_FUNC(__imp__sub_820F1200);
PPC_EXTERN_FUNC(__imp__sub_8211B9A8);

// the editor's start builds the archetype library (r3, its root XmlNodeRef at +0) right after reading the catalogue
PPC_FUNC(sub_821AA1D8) {
    uint32_t holder = uint32_t(ctx.r[3]);
    __imp__sub_821AA1D8(ctx, base);
    uint32_t library = R32(holder);
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    if (!library || !g_scratch) return;
    OpenCampaignPacks(ctx);
    AdoptCampaignEnemies(ctx, library);
    ReplaceMultiplayerItems(ctx);
}

// spawns an editor object: r4 the catalogue key, which names the class; ours are archetypes, so r4 gets their class
PPC_FUNC(sub_820F1200) {
    if (const EnemyType* e = EnemyByKey(ReadCString(uint32_t(ctx.r[4])))) ctx.r[4] = GuestText(e->cls);
    __imp__sub_820F1200(ctx, base);
}

// a string of the editor's table: r3 the std::wstring out, r5 the section, r6 the key
PPC_FUNC(sub_8211B9A8) {
    uint32_t out = uint32_t(ctx.r[3]);
    bool enemies = GuestString(uint32_t(ctx.r[5])) == "ObjectCategoryNames" &&
                   GuestString(uint32_t(ctx.r[6])) == "MultiplayerItems";
    __imp__sub_8211B9A8(ctx, base);
    if (enemies) CallGuest(ctx, SYM_WString_Assign, {out, fcip::ui::Text(kEnemiesCaption)});
}
