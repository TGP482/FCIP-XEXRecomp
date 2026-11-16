// ige.xex, the solo level editor: the Equipment tab edits what the kept mission gives out. As a mission loads
// (8229CD10) its PlayerEquipPack names the player's pack (821394C0), each <EquipPack> becomes an entry of the script
// table EquipPacks (82139080: its Items in order as {Type, Name}, the primary one marked, and its Ammo), each
// <Weapons><Used><Weapon Name> is loaded (8220BD38, once per weapon) and <Weapons><Ammo> becomes the script table
// MaxAmmo. Enemies name their packs in equipEquipment and equipDropPack. An edit changes the kept node and gives the
// game the same values; undo puts a copy of the edited node back.
#include "leveleditor/equipment.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/fields.h"
#include "leveleditor/guest.h"
#include "leveleditor/mission.h"
#include "leveleditor/property_lists.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr const char* kPlayerPackKey = "PlayerEquipPack";
constexpr const char* kEquipPacks = "EquipPacks";
constexpr const char* kEquipPack = "EquipPack";
constexpr const char* kPackName = "name";
constexpr const char* kPrimary = "primary";
constexpr const char* kItems = "Items";
constexpr const char* kItemType = "type";
constexpr const char* kAmmo = "Ammo";
constexpr const char* kWeapons = "Weapons";
constexpr const char* kUsed = "Used";
constexpr const char* kWeapon = "Weapon";
constexpr const char* kWeaponName = "Name";

constexpr uint32_t kGameWeapons = 0x338;  // the game's weapon system

// guest scratch: two texts for node calls, a node reference, then the MaxAmmo script
constexpr uint32_t kScratchKey = 0x000;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = 0x040;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchRef = 0x140;
constexpr uint32_t kScratchScript = 0x150;
constexpr uint32_t kScratchScriptBytes = 0x800;
constexpr uint32_t kScratchBytes = kScratchScript + kScratchScriptBytes;

constexpr size_t kNameChars = 64;

struct Item {
    std::string name;
    std::string type;
};
struct Pack {
    std::string name;
    std::string primary;
    std::vector<Item> items;
    std::vector<XmlAttribute> ammo;
};
struct Snapshot {
    bool kept = false;
    std::string player;
    std::vector<Pack> packs;
    std::vector<std::string> used;
    std::vector<XmlAttribute> max_ammo;
    uint32_t generation = 0;
};
using Edit = std::function<void(PPCContext&)>;
using Change = std::function<void(PPCContext&, uint32_t)>;

std::mutex g_lock;
Snapshot g_snapshot;
std::vector<Edit> g_edits;
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown = 0;
bool g_stale = false;
// window thread
Snapshot g_view;
char g_new_pack[kNameChars] = {};

// Editor thread

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

std::string Tag(uint32_t node) { return GuestString(node + kNodeTag); }

std::vector<XmlAttribute> Attributes(uint32_t node) {
    std::vector<XmlAttribute> out;
    if (!node) return out;
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        out.push_back({GuestString(a + kAttrKey), GuestString(a + kAttrValue)});
    }
    return out;
}

std::string Find(const std::vector<XmlAttribute>& attributes, const std::string& key) {
    for (const XmlAttribute& a : attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

uint32_t ChildTagged(uint32_t node, const std::string& tag) {
    for (uint32_t i = 0; node && i < ChildCount(node); i++) {
        if (Tag(ChildAt(node, i)) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t PackNamed(uint32_t packs, const std::string& name) {
    for (uint32_t i = 0; packs && i < ChildCount(packs); i++) {
        uint32_t pack = ChildAt(packs, i);
        if (Tag(pack) == kEquipPack && Find(Attributes(pack), kPackName) == name) return pack;
    }
    return 0;
}

void SetAttr(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

// a new child of parent, which keeps it
uint32_t NewChild(PPCContext& ctx, uint32_t parent, const std::string& tag) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(parent, kNodeNewChild), {g_scratch + kScratchRef, parent, KeyText(tag)});
    uint32_t child = R32(g_scratch + kScratchRef);
    if (child) CallGuest(ctx, Method(child, kNodeRelease), {child});
    return child;
}

void RemoveChild(PPCContext& ctx, uint32_t parent, uint32_t child) {
    CallGuest(ctx, Method(child, kNodeAddRef), {child});
    W32(g_scratch + kScratchRef, child);
    CallGuest(ctx, Method(parent, kNodeRemoveChild), {parent, g_scratch + kScratchRef});
}

void Release(PPCContext& ctx, uint32_t node) { CallGuest(ctx, Method(node, kNodeRelease), {node}); }

// a deep copy, referenced once
uint32_t Clone(PPCContext& ctx, uint32_t node) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(node, kNodeClone), {g_scratch + kScratchRef, node});
    return R32(g_scratch + kScratchRef);
}

// MaxAmmo as the mission load sets it, by a script
void LoadMaxAmmo(PPCContext& ctx, uint32_t weapons) {
    std::string script = "MaxAmmo = MaxAmmo or {}\n";
    for (const XmlAttribute& a : Attributes(ChildTagged(weapons, kAmmo))) {
        script += "MaxAmmo." + a.key + " = " + std::to_string(std::atoi(a.value.c_str())) + "\n";
    }
    uint32_t scripts = R32(SYM_ScriptSystem_pointer);
    if (!scripts || script.size() >= kScratchScriptBytes) return;
    uint32_t text = WriteText(g_scratch + kScratchScript, script, kScratchScriptBytes);
    CallGuest(ctx, SYM_Script_ExecuteBuffer, {scripts, text, uint32_t(script.size())});
}

// the game takes the mission's equipment as it loads it
void Load(PPCContext& ctx) {
    uint32_t mission = KeptMission();
    uint32_t game = R32(SYM_Game_pointer);
    if (!mission || !game) return;
    uint32_t packs = ChildTagged(mission, kEquipPacks);
    for (uint32_t i = 0; packs && i < ChildCount(packs); i++) {
        W32(g_scratch + kScratchRef, ChildAt(packs, i));
        CallGuest(ctx, SYM_EquipPack_Load, {game, g_scratch + kScratchRef});
    }
    uint32_t weapons = ChildTagged(mission, kWeapons);
    uint32_t used = ChildTagged(weapons, kUsed);
    for (uint32_t i = 0; used && i < ChildCount(used); i++) {
        std::string name = Find(Attributes(ChildAt(used, i)), kWeaponName);
        CallGuest(ctx, SYM_Weapons_Load, {R32(game + kGameWeapons), ValueText(name)});
    }
    if (weapons) LoadMaxAmmo(ctx, weapons);
}

// one of the mission's nodes changed, one step of undo that puts a copy of it as it was back
void EditNode(PPCContext& ctx, const char* tag, const std::string& label, const Change& change) {
    uint32_t node = ChildTagged(KeptMission(), tag);
    uint32_t before = node ? Clone(ctx, node) : 0;
    if (!before) return;
    change(ctx, node);
    Load(ctx);
    g_stale = true;
    NoteMapChanged();
    PushUndo(
        ctx, label,
        [node, before](PPCContext& ctx) {
            SwapContent(node, before);
            Release(ctx, before);
            Load(ctx);
            g_stale = true;
        },
        [before](PPCContext& ctx) { Release(ctx, before); });
}

void SetPlayerPack(PPCContext& ctx, const std::string& name, bool undoable) {
    uint32_t mission = KeptMission();
    uint32_t game = R32(SYM_Game_pointer);
    if (!mission) return;
    std::string old = Find(Attributes(mission), kPlayerPackKey);
    SetAttr(ctx, mission, kPlayerPackKey, name);
    if (game) CallGuest(ctx, SYM_Game_SetPlayerEquipPack, {game, ValueText(name)});
    g_stale = true;
    NoteMapChanged();
    if (!undoable || old == name) return;
    PushUndo(ctx, "Set the player's equip pack", [old](PPCContext& ctx) { SetPlayerPack(ctx, old, false); });
}

Snapshot ReadEquipment() {
    Snapshot s;
    uint32_t mission = KeptMission();
    s.kept = mission != 0;
    s.player = Find(Attributes(mission), kPlayerPackKey);
    uint32_t packs = ChildTagged(mission, kEquipPacks);
    for (uint32_t i = 0; packs && i < ChildCount(packs); i++) {
        uint32_t node = ChildAt(packs, i);
        std::vector<XmlAttribute> attributes = Attributes(node);
        Pack p{Find(attributes, kPackName), Find(attributes, kPrimary), {}, Attributes(ChildTagged(node, kAmmo))};
        uint32_t items = ChildTagged(node, kItems);
        for (uint32_t j = 0; items && j < ChildCount(items); j++) {
            uint32_t item = ChildAt(items, j);
            p.items.push_back({Tag(item), Find(Attributes(item), kItemType)});
        }
        s.packs.push_back(std::move(p));
    }
    uint32_t weapons = ChildTagged(mission, kWeapons);
    uint32_t used = ChildTagged(weapons, kUsed);
    for (uint32_t i = 0; used && i < ChildCount(used); i++) {
        s.used.push_back(Find(Attributes(ChildAt(used, i)), kWeaponName));
    }
    s.max_ammo = Attributes(ChildTagged(weapons, kAmmo));
    return s;
}

// Edits queued by the panel

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

void QueuePackEdit(std::string pack, std::string label, std::function<void(PPCContext&, uint32_t)> change) {
    Queue([=](PPCContext& ctx) {
        EditNode(ctx, kEquipPacks, label, [&](PPCContext& ctx, uint32_t packs) {
            if (uint32_t node = PackNamed(packs, pack)) change(ctx, node);
        });
    });
}

void SetPackAmmo(const std::string& pack, const std::string& key, const std::string& value) {
    QueuePackEdit(pack, "Set " + key + " ammo of " + pack, [=](PPCContext& ctx, uint32_t node) {
        if (uint32_t ammo = ChildTagged(node, kAmmo)) SetAttr(ctx, ammo, key, value);
    });
}

void SetPrimary(const std::string& pack, const std::string& item) {
    QueuePackEdit(pack, "Set the primary of " + pack, [=](PPCContext& ctx, uint32_t node) {
        SetAttr(ctx, node, kPrimary, item);
    });
}

void AddItem(const std::string& pack, const EquipItem& item) {
    std::string name = item.name;
    std::string type = item.type;
    QueuePackEdit(pack, "Add " + name + " to " + pack, [=](PPCContext& ctx, uint32_t node) {
        uint32_t items = ChildTagged(node, kItems);
        if (!items) items = NewChild(ctx, node, kItems);
        if (uint32_t added = items ? NewChild(ctx, items, name) : 0) SetAttr(ctx, added, kItemType, type);
    });
}

void RemoveItem(const std::string& pack, int index) {
    QueuePackEdit(pack, "Remove an item from " + pack, [=](PPCContext& ctx, uint32_t node) {
        uint32_t items = ChildTagged(node, kItems);
        if (items && uint32_t(index) < ChildCount(items)) RemoveChild(ctx, items, ChildAt(items, uint32_t(index)));
    });
}

void DeletePack(const std::string& pack) {
    Queue([=](PPCContext& ctx) {
        EditNode(ctx, kEquipPacks, "Delete the equip pack " + pack, [&](PPCContext& ctx, uint32_t packs) {
            if (uint32_t node = PackNamed(packs, pack)) RemoveChild(ctx, packs, node);
        });
    });
}

// a new pack with no items and no ammo
void CreatePack(const std::string& name) {
    Queue([=](PPCContext& ctx) {
        EditNode(ctx, kEquipPacks, "Create the equip pack " + name, [&](PPCContext& ctx, uint32_t packs) {
            if (PackNamed(packs, name)) return;
            uint32_t pack = NewChild(ctx, packs, kEquipPack);
            if (!pack) return;
            SetAttr(ctx, pack, kPackName, name);
            SetAttr(ctx, pack, kPrimary, "");
            NewChild(ctx, pack, kItems);
            uint32_t ammo = NewChild(ctx, pack, kAmmo);
            for (const char* a : kAmmoNames) {
                if (ammo) SetAttr(ctx, ammo, a, "0");
            }
        });
    });
}

void QueueWeaponsEdit(std::string label, std::function<void(PPCContext&, uint32_t)> change) {
    Queue([=](PPCContext& ctx) { EditNode(ctx, kWeapons, label, change); });
}

void AddUsed(const std::string& name) {
    QueueWeaponsEdit("Preload " + name, [=](PPCContext& ctx, uint32_t weapons) {
        uint32_t used = ChildTagged(weapons, kUsed);
        if (!used) used = NewChild(ctx, weapons, kUsed);
        if (uint32_t added = used ? NewChild(ctx, used, kWeapon) : 0) SetAttr(ctx, added, kWeaponName, name);
    });
}

void RemoveUsed(int index) {
    QueueWeaponsEdit("Stop preloading a weapon", [=](PPCContext& ctx, uint32_t weapons) {
        uint32_t used = ChildTagged(weapons, kUsed);
        if (used && uint32_t(index) < ChildCount(used)) RemoveChild(ctx, used, ChildAt(used, uint32_t(index)));
    });
}

void SetMaxAmmo(const std::string& key, const std::string& value) {
    QueueWeaponsEdit("Set " + key + " max ammo", [=](PPCContext& ctx, uint32_t weapons) {
        if (uint32_t ammo = ChildTagged(weapons, kAmmo)) SetAttr(ctx, ammo, key, value);
    });
}

// Panel, window thread

void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void Row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 10);
    ImGui::SetNextItemWidth(-1);
}

// one int per ammo kind; set(key, value) queues the edit
void DrawAmmo(const std::vector<XmlAttribute>& ammo, const std::function<void(std::string, std::string)>& set) {
    for (const XmlAttribute& a : ammo) {
        Row(a.key.c_str());
        std::string edited;
        if (DrawField(a.key.c_str(), FieldKind::Int, a.value, edited)) set(a.key, edited);
    }
}

void DrawPlayerPack() {
    Row("Player's pack");
    if (!ImGui::BeginCombo("##player", g_view.player.c_str())) return;
    for (const Pack& p : g_view.packs) {
        if (!ImGui::Selectable(p.name.c_str(), p.name == g_view.player)) continue;
        std::string name = p.name;
        Queue([name](PPCContext& ctx) { SetPlayerPack(ctx, name, true); });
    }
    ImGui::EndCombo();
}

void DrawItems(const Pack& p) {
    for (size_t i = 0; i < p.items.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) RemoveItem(p.name, int(i));
        ImGui::SameLine();
        ImGui::Text("%s (%s)", p.items[i].name.c_str(), p.items[i].type.c_str());
        ImGui::PopID();
    }
    Row("Add item");
    if (ImGui::BeginCombo("##add", "pick an item")) {
        for (const EquipItem& item : kEquipItems) {
            if (ImGui::Selectable(item.name)) AddItem(p.name, item);
        }
        ImGui::EndCombo();
    }
    Row(kPrimary);
    if (ImGui::BeginCombo("##primary", p.primary.c_str())) {
        for (const Item& item : p.items) {
            if (ImGui::Selectable(item.name.c_str(), item.name == p.primary)) SetPrimary(p.name, item.name);
        }
        ImGui::EndCombo();
    }
}

void DrawPack(const Pack& p) {
    if (!ImGui::CollapsingHeader(p.name.c_str())) return;
    ImGui::PushID(p.name.c_str());
    DrawItems(p);
    std::string pack = p.name;
    DrawAmmo(p.ammo, [pack](std::string key, std::string value) { SetPackAmmo(pack, key, value); });
    if (ImGui::Button("Delete Pack")) DeletePack(p.name);
    ImGui::PopID();
}

void DrawNewPack() {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##newpack", "new pack name", g_new_pack, sizeof(g_new_pack));
    ImGui::SameLine();
    std::string name = g_new_pack;
    bool taken = false;
    for (const Pack& p : g_view.packs) taken = taken || p.name == name;
    ImGui::BeginDisabled(name.empty() || taken);
    if (ImGui::Button("Create Pack")) {
        CreatePack(name);
        g_new_pack[0] = 0;
    }
    ImGui::EndDisabled();
}

void DrawWeapons() {
    for (size_t i = 0; i < g_view.used.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Remove")) RemoveUsed(int(i));
        ImGui::SameLine();
        ImGui::TextUnformatted(g_view.used[i].c_str());
        ImGui::PopID();
    }
    Row("Preload");
    if (ImGui::BeginCombo("##preload", "pick a weapon")) {
        for (const char* w : kWeaponNames) {
            bool used = false;
            for (const std::string& u : g_view.used) used = used || u == w;
            if (!used && ImGui::Selectable(w)) AddUsed(w);
        }
        ImGui::EndCombo();
    }
}

}  // namespace

void fcip::editor::UpdateEquipment(PPCContext& ctx) {
    if (!EnsureScratch()) return;
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    if (KeptMission() == g_shown && !g_stale) return;
    Snapshot s = ReadEquipment();
    g_shown = KeptMission();
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

void fcip::editor::DrawEquipmentPanel() {
    {
        std::lock_guard lock(g_lock);
        if (g_snapshot.generation != g_view.generation) g_view = g_snapshot;
    }
    ImGui::TextUnformatted("EQUIPMENT");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("Equip packs, preloaded weapons and max ammo, saved with the level");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!g_view.kept) {
        ImGui::TextUnformatted("Only a campaign or custom level keeps its mission's equipment.");
        return;
    }
    ImGui::BeginChild("##equipment");
    DrawPlayerPack();
    ImGui::Spacing();
    Heading("Equip Packs");
    for (const Pack& p : g_view.packs) DrawPack(p);
    DrawNewPack();
    ImGui::Spacing();
    Heading("Preloaded Weapons");
    DrawWeapons();
    ImGui::Spacing();
    Heading("Max Ammo");
    DrawAmmo(g_view.max_ammo, [](std::string key, std::string value) { SetMaxAmmo(key, value); });
    ImGui::EndChild();
}
