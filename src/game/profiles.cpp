// default.xex: the original Xbox profile list (P_Profile), left unopened by the 360 build, opened by OPTIONS again;
// the first start names a profile and each one saves to profiles\<name>.bin, the current also to the gamer's settings,
// and keeps its multiplayer stats in profiles\<name>.stats and its achievements in profiles\<name>.achievements
#include "game/profiles.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/settings.h"
#include "core/log.h"
#include "game/virtual_keyboard.h"
#include "kernel/memory.h"
#include "xam/xam.h"
#include "xdk/hle.h"

using namespace kernel;
namespace fs = std::filesystem;

namespace {

// the profile manager: a vector of profiles and the current one
constexpr uint32_t kManagerBegin = 0x04;
constexpr uint32_t kManagerEnd = 0x08;
constexpr uint32_t kManagerCurrent = 0x14;
constexpr uint32_t kMaxProfiles = 10;  // the list's NEW PROFILE row goes at this many

// a profile (constructor 821E06B0)
constexpr uint32_t kProfileBytes = 0x3218;
constexpr uint32_t kProfileValid = 0x01;  // u8
constexpr uint32_t kProfileName = 0x02;   // UTF-16
constexpr uint32_t kProfileUser = 0x8C;   // the user whose title settings it saves to
constexpr uint32_t kProfileSignedIn = 0x90;  // its user's sign in state (821E1840); story ends award only when set
constexpr uint32_t kSignedInLocally = 1;     // the chosen profile is the signed in gamer
// what the save writes as title settings: a dword, the options (0x388) and the packed progress (0x3BF)
constexpr uint32_t kSavedStart = 0x94;
constexpr uint32_t kSavedBytes = 0x74B;
constexpr uint32_t kOptionsOffset = 4;
constexpr uint32_t kOptionsBytes = 0x388;
constexpr uint32_t kProgressOffset = kOptionsOffset + kOptionsBytes;
constexpr uint32_t kProgressBytes = 0x3BF;

// the title settings the loaders read, records in a results block
constexpr uint32_t kOptionsSetting = 0x63E83FFF;
constexpr uint32_t kProgressSetting = 0x63E83FFE;
constexpr uint32_t kResultsRecords = 0x04;
constexpr uint32_t kResultsCount = 0x08;
constexpr uint32_t kRecordId = 0x10;
constexpr uint32_t kRecordSize = 0x20;
constexpr uint32_t kRecordData = 0x24;
constexpr uint32_t kRecordBytes = 0x30;

// CryProfile, the profile menu
constexpr uint32_t kMenuSystem = 0x10;
constexpr uint32_t kMenuPages = 0x50;  // P_Profile first, then P_EditProfile
constexpr uint32_t kMenuShowPage = 0x2C;  // vtable: (menu, page, 1, 1)
constexpr uint32_t kSystemUi = 0x18;
constexpr uint32_t kUiActivePages = 0x38;
constexpr uint32_t kEventPressA = 0x2F;

constexpr uint32_t kUserPad = 0x24C;  // the user manager's player: the pad that pressed START
constexpr uint32_t kOtherUsers = 0x100;  // profiles other than the current one save to no user

constexpr uint32_t kMaxName = 15;
constexpr const char* kDefaultName = "JACK CARVER";
constexpr const char* kCreateTitle = "CREATE A PROFILE";
constexpr const char* kNameTaken = "A PROFILE WITH THIS NAME EXISTS";
constexpr const char* kNameRefused = "THIS NAME CANNOT BE USED";

bool g_main_seen = false;
bool g_asked = false;
uint32_t g_menu = 0;  // the profile menu the last NEW PROFILE came from
uint32_t g_next_user = kOtherUsers;

// guest scratch: UTF-16 texts, two std::wstrings and a title settings results block with its two records and data
constexpr uint32_t kTextUnits = 0x40;
constexpr uint32_t kTextSlots = 4;
constexpr uint32_t kStatusSlot = 3;
constexpr uint32_t kWideStringBytes = 0x20;
constexpr uint32_t kWideStrings = kTextSlots * kTextUnits * 2;
constexpr uint32_t kResults = kWideStrings + 2 * kWideStringBytes;
constexpr uint32_t kResultsList = kResults + 0x10;
constexpr uint32_t kRecords = kResultsList + 0x10;
constexpr uint32_t kResultsData = kRecords + 2 * kRecordBytes;
constexpr uint32_t kScratchBytes = kResultsData + kSavedBytes;
uint32_t g_scratch = 0;

uint32_t Scratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch;
}

uint32_t GuestText(uint32_t slot, const std::string& text) {
    uint32_t addr = Scratch() + slot * kTextUnits * 2;
    uint32_t n = std::min<uint32_t>(uint32_t(text.size()), kTextUnits - 1);
    for (uint32_t i = 0; i < n; i++) W16(addr + i * 2, uint8_t(text[i]));
    W16(addr + n * 2, 0);
    return addr;
}

// a std::wstring over a slot's text, the first slot's in the first string; a capacity of 8 or more keeps its pointer
uint32_t GuestWideString(uint32_t slot, const std::string& text) {
    uint32_t str = Scratch() + kWideStrings + (slot ? kWideStringBytes : 0);
    W32(str + 0x04, GuestText(slot, text));
    W32(str + 0x14, uint32_t(text.size()));
    W32(str + 0x18, std::max<uint32_t>(uint32_t(text.size()), 8));
    return str;
}

std::string Narrow(uint32_t text, uint32_t length) {
    std::string out;
    for (uint32_t i = 0; i < length; i++) {
        uint16_t c = R16(text + i * 2);
        out += c < 0x80 ? char(c) : '?';
    }
    return out;
}

std::string ProfileName(uint32_t profile) {
    uint32_t text = profile + kProfileName;
    uint32_t n = 0;
    while (n < kTextUnits && R16(text + n * 2)) n++;
    return Narrow(text, n);
}

uint32_t Manager() { return R32(SYM_ProfileManager_pointer); }

std::vector<uint32_t> Loaded() {
    uint32_t manager = Manager();
    std::vector<uint32_t> out;
    if (!manager) return out;
    uint32_t end = R32(manager + kManagerEnd);
    for (uint32_t it = R32(manager + kManagerBegin); it && it < end; it += 4) out.push_back(R32(it));
    return out;
}

fs::path Folder() { return app::ExeDir() / "profiles"; }
fs::path FileFor(const std::string& name) { return Folder() / (name + ".bin"); }

// the stats and achievements the game reads and writes are the profile in play's
void UseProfileFiles(const std::string& name) {
    xam::UseStatsFile(name.empty() ? std::string() : (Folder() / (name + ".stats")).string());
    xam::UseAchievementsFile(name.empty() ? std::string() : (Folder() / (name + ".achievements")).string());
}

// a gamertag: a letter first, then letters, digits and single spaces, none at the end
bool NameAllowed(const std::string& name) {
    if (name.empty() || !std::isalpha(uint8_t(name[0])) || name.back() == ' ') return false;
    for (size_t i = 1; i < name.size(); i++) {
        bool space = name[i] == ' ';
        if (!space && !std::isalnum(uint8_t(name[i]))) return false;
        if (space && name[i - 1] == ' ') return false;
    }
    return true;
}

// by a saved or loaded profile other than self
bool NameTaken(const std::string& name, uint32_t self) {
    std::error_code ec;
    if (self && ProfileName(self) == name) return false;
    if (fs::exists(FileFor(name), ec)) return true;
    for (uint32_t profile : Loaded()) {
        if (ProfileName(profile) == name) return true;
    }
    return false;
}

// the saved block as the last save packed it
void WriteProfile(uint32_t profile) {
    if (fcip::Get().profile.empty()) return;
    std::error_code ec;
    fs::create_directories(Folder(), ec);
    std::ofstream out(FileFor(ProfileName(profile)), std::ios::binary);
    out.write(reinterpret_cast<const char*>(HostPtr(profile + kSavedStart)), kSavedBytes);
    if (!out) LOGE("profiles: %s could not be saved", ProfileName(profile).c_str());
}

void SetRecord(uint32_t record, uint32_t id, uint32_t size, uint32_t data) {
    W32(record + kRecordId, id);
    W32(record + kRecordSize, size);
    W32(record + kRecordData, data);
}

// a saved file through the game's own title settings loaders, as if the gamer's settings held it
void LoadProfile(PPCContext& ctx, uint32_t profile, const std::vector<char>& bytes) {
    uint32_t base = Scratch();
    uint32_t data = base + kResultsData;
    std::memcpy(HostPtr(data), bytes.data(), kSavedBytes);
    uint32_t options = base + kRecords;
    uint32_t progress = options + kRecordBytes;
    SetRecord(options, kOptionsSetting, kOptionsBytes, data + kOptionsOffset);
    SetRecord(progress, kProgressSetting, kProgressBytes, data + kProgressOffset);
    W32(base + kResultsList, options);
    W32(base + kResultsList + 4, progress);
    W32(base + kResults + kResultsRecords, base + kResultsList);
    W32(base + kResults + kResultsCount, 2);
    CallGuest(ctx, SYM_Profile_LoadOptions, {Manager(), profile, base + kResults});
    CallGuest(ctx, SYM_Profile_LoadProgress, {Manager(), profile, base + kResults});
    std::memcpy(HostPtr(profile + kSavedStart), bytes.data(), 4);
}

uint32_t NewProfile(PPCContext& ctx, const std::string& name) {
    uint32_t profile = uint32_t(CallGuest(ctx, SYM_Guest_Alloc, {kProfileBytes}));
    if (!profile) return 0;
    CallGuest(ctx, SYM_Profile_New, {profile, GuestWideString(0, name)});
    W8(profile + kProfileValid, 1);
    W32(profile + kProfileUser, g_next_user++);
    CallGuest(ctx, SYM_ProfileManager_Add, {Manager(), profile});
    return profile;
}

// every saved profile the list does not hold yet
void LoadSaved(PPCContext& ctx) {
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(Folder(), ec)) {
        std::string name = e.path().stem().string();
        if (e.path().extension() != ".bin" || Loaded().size() >= kMaxProfiles) continue;
        bool loaded = false;
        for (uint32_t profile : Loaded()) loaded = loaded || ProfileName(profile) == name;
        if (loaded) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::vector<char> bytes(std::istreambuf_iterator<char>(in), {});
        if (bytes.size() != kSavedBytes) continue;
        if (uint32_t profile = NewProfile(ctx, name)) LoadProfile(ctx, profile, bytes);
    }
}

uint32_t UserPad() {
    uint32_t users = R32(SYM_UserManager_pointer);
    uint32_t player = users ? R32(users + 8) : 0;
    return player ? R32(player + kUserPad) : 0;
}

// the gamer's title settings and the pad now belong to the chosen profile
void SetCurrent(PPCContext& ctx, uint32_t profile) {
    uint32_t manager = Manager();
    uint32_t current = R32(manager + kManagerCurrent);
    if (!profile || profile == current) return;
    uint32_t user = UserPad();
    if (current) {
        CallGuest(ctx, SYM_Profile_Save, {current});
        W32(current + kProfileUser, g_next_user++);
        W32(current + kProfileSignedIn, 0);
    }
    W32(profile + kProfileUser, user);
    W32(profile + kProfileSignedIn, kSignedInLocally);
    CallGuest(ctx, SYM_ProfileManager_Select, {manager, user});
    CallGuest(ctx, SYM_ProfileManager_Apply, {manager});
    fcip::Get().profile = ProfileName(profile);
    UseProfileFiles(fcip::Get().profile);
    app::Save();
    CallGuest(ctx, SYM_Profile_Save, {profile});
}

uint32_t ListPage(uint32_t menu) { return R32(R32(menu + kMenuPages)); }

void RefreshList(PPCContext& ctx, uint32_t menu) {
    if (menu) CallGuest(ctx, SYM_ProfileMenu_PageShown, {menu, ListPage(menu)});
}

// the selected row's profile, 0 on NEW PROFILE
uint32_t Selected(PPCContext& ctx, uint32_t menu) {
    int32_t index = int32_t(CallGuest(ctx, SYM_ProfileList_Row, {menu}));
    std::vector<uint32_t> loaded = Loaded();
    return index >= 0 && uint32_t(index) < loaded.size() ? loaded[index] : 0;
}

// a typed name the profiles can take; else the line above says why
bool CheckName(PPCContext& ctx, const std::string& name, uint32_t self) {
    const char* refusal = nullptr;
    if (!NameAllowed(name)) refusal = kNameRefused;
    else if (NameTaken(name, self)) refusal = kNameTaken;
    if (refusal) fcip::keyboard::SetInfo(ctx, GuestText(1, refusal));
    return !refusal;
}

bool OnFirstName(PPCContext& ctx, uint32_t text, uint32_t length) {
    std::string name = Narrow(text, length);
    uint32_t manager = Manager();
    uint32_t current = R32(manager + kManagerCurrent);
    if (!CheckName(ctx, name, current)) return false;
    if (!current) return true;
    for (size_t i = 0; i <= name.size(); i++) W16(current + kProfileName + uint32_t(i) * 2, uint8_t(name.c_str()[i]));
    fcip::Get().profile = name;
    UseProfileFiles(name);
    app::Save();
    CallGuest(ctx, SYM_Profile_Save, {current});
    CallGuest(ctx, SYM_ProfileManager_Apply, {manager});
    LOGR("profiles: created %s", name.c_str());
    return true;
}

bool OnNewName(PPCContext& ctx, uint32_t text, uint32_t length) {
    std::string name = Narrow(text, length);
    if (!CheckName(ctx, name, 0)) return false;
    uint32_t current = R32(Manager() + kManagerCurrent);
    if (fcip::Get().profile.empty() && current) {
        fcip::Get().profile = ProfileName(current);
        UseProfileFiles(fcip::Get().profile);
        app::Save();
        CallGuest(ctx, SYM_Profile_Save, {current});
    }
    if (uint32_t profile = NewProfile(ctx, name)) {
        CallGuest(ctx, SYM_Profile_Pack, {profile});
        WriteProfile(profile);
        LOGR("profiles: created %s", name.c_str());
    }
    RefreshList(ctx, g_menu);
    return true;
}

bool AskName(PPCContext& ctx, fcip::keyboard::Accept accept) {
    return fcip::keyboard::Open(ctx, GuestText(0, kDefaultName), GuestText(1, kCreateTitle), kMaxName, accept);
}

bool ListShown(PPCContext& ctx, uint32_t menu) {
    uint32_t ui = R32(R32(menu + kMenuSystem) + kSystemUi);
    return CallGuest(ctx, SYM_UIInputStack_Top, {ui + kUiActivePages, 0, 0}) == ListPage(menu);
}

}  // namespace

void fcip::profiles::NoteMainMenu() {
    if (!g_main_seen) UseProfileFiles(Get().profile);
    g_main_seen = true;
}

void fcip::profiles::Tick(PPCContext& ctx) {
    if (!g_main_seen || g_asked || !Get().profile.empty()) return;
    uint32_t manager = Manager();
    if (!manager || !R32(manager + kManagerCurrent)) return;
    g_asked = AskName(ctx, OnFirstName);
}

PPC_EXTERN_FUNC(GUEST_ORIG(Profile_Save));
PPC_EXTERN_FUNC(GUEST_ORIG(ProfileManager_Remove));
PPC_EXTERN_FUNC(GUEST_ORIG(ProfileList_AskDelete));
PPC_EXTERN_FUNC(GUEST_ORIG(ProfileList_Key));
PPC_EXTERN_FUNC(GUEST_ORIG(ProfileMenu_PageShown));
PPC_EXTERN_FUNC(GUEST_ORIG(XUserGetName));
PPC_EXTERN_FUNC(GUEST_ORIG(LiveStatus_Set));

// (profile) packs it and writes the gamer's title settings
PPC_FUNC(GUEST(Profile_Save)) {
    uint32_t profile = uint32_t(ctx.r[3]);
    GUEST_ORIG(Profile_Save)(ctx, base);
    WriteProfile(profile);
}

// (manager, index)
PPC_FUNC(GUEST(ProfileManager_Remove)) {
    std::vector<uint32_t> loaded = Loaded();
    uint32_t index = uint32_t(ctx.r[4]);
    std::string name = index < loaded.size() ? ProfileName(loaded[index]) : std::string();
    GUEST_ORIG(ProfileManager_Remove)(ctx, base);
    std::error_code ec;
    if (ctx.r[3] && !name.empty()) fs::remove(FileFor(name), ec);
}

// (menu, profile): X on a row; the current profile stays
PPC_FUNC(GUEST(ProfileList_AskDelete)) {
    if (uint32_t(ctx.r[4]) == R32(Manager() + kManagerCurrent)) return;
    GUEST_ORIG(ProfileList_AskDelete)(ctx, base);
}

// (menu): Y on a row
PPC_FUNC(GUEST(ProfileList_SetCurrent)) {
    uint32_t menu = uint32_t(ctx.r[3]);
    SetCurrent(ctx, Selected(ctx, menu));
    RefreshList(ctx, menu);
    ctx.r[3] = 1;
}

// (menu, sender, event): A, lost from the 360 build, names a new profile or edits the row's
PPC_FUNC(GUEST(ProfileList_Key)) {
    uint32_t menu = uint32_t(ctx.r[3]);
    uint64_t sender = ctx.r[4];
    uint64_t event = ctx.r[5];
    bool press = R32(uint32_t(event)) == kEventPressA;
    if (!press || !ListShown(ctx, menu)) {
        ctx.r[3] = menu;
        ctx.r[4] = sender;
        ctx.r[5] = event;
        GUEST_ORIG(ProfileList_Key)(ctx, base);
        return;
    }
    uint32_t profile = Selected(ctx, menu);
    if (!profile) {
        g_menu = menu;
        if (Loaded().size() < kMaxProfiles) AskName(ctx, OnNewName);
    } else {
        SetCurrent(ctx, profile);
        CallGuest(ctx, R32(R32(menu) + kMenuShowPage), {menu, 1, 1, 1});
    }
    ctx.r[3] = 0;
}

// (menu, page): the list fills from the loaded profiles
PPC_FUNC(GUEST(ProfileMenu_PageShown)) {
    uint32_t menu = uint32_t(ctx.r[3]);
    uint32_t page = uint32_t(ctx.r[4]);
    if (page == ListPage(menu)) LoadSaved(ctx);
    ctx.r[3] = menu;
    ctx.r[4] = page;
    GUEST_ORIG(ProfileMenu_PageShown)(ctx, base);
}

// (user, text, size): the current profile's name in place of the gamertag
PPC_FUNC(GUEST(XUserGetName)) {
    const std::string& name = fcip::Get().profile;
    uint32_t size = uint32_t(ctx.r[5]);
    if (name.empty() || !size) {
        GUEST_ORIG(XUserGetName)(ctx, base);
        return;
    }
    uint32_t text = uint32_t(ctx.r[4]);
    uint32_t n = std::min<uint32_t>(uint32_t(name.size()), size - 1);
    std::memcpy(HostPtr(text), name.data(), n);
    W8(text + n, 0);
    ctx.r[3] = 0;
}

// before CryProfile shows its first page: the list, as the original's OPTIONS did, not P_EditProfile
void ProfileListFirst(PPCContext& ctx, uint8_t*) { ctx.r[4] = 0; }

// (help bar, page, icon name, text): the 360 blanks the original's NOT SIGNED IN corner every frame; it shows the
// current profile's name, the gamertag the game reads
PPC_FUNC(GUEST(LiveStatus_Set)) {
    uint32_t manager = Manager();
    uint32_t current = manager ? R32(manager + kManagerCurrent) : 0;
    if (current) ctx.r[6] = GuestWideString(kStatusSlot, ProfileName(current));
    GUEST_ORIG(LiveStatus_Set)(ctx, base);
}
