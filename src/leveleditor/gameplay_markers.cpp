// ige.xex, the solo level editor: the level's gameplay objects, as their scripts (lua.dat) use their Properties.
// Ladders, water currents, damage boxes, explosives and automatic doors work inside the box their script's
// SetLocalBBox gives them (the engine's GetBBox, as the triggers'). A water current pushes along vector_Direction, or
// along the entity's facing with bUseEntityOrientation, at fSpeed. A zipline runs from its fromTagPoint to its
// toTagPoint (Game:GetTagPoint). A DamageArea hurts inside the areas naming it, a DamagingAnimation within Radius.
// A Waypoint is an objective marker of Radius (SetRadius) that its WaypointTag and CompletionTag name. Pickups,
// secrets among them, and mounted weapons are where they stand.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "leveleditor/markers.h"

using namespace fcip::editor;

namespace {

// the box SetLocalBBox gives each, and the Properties named under it
struct BoxClass {
    const char* cls;
    const char* shown[3];
};
constexpr BoxClass kBoxClasses[] = {
    {"Ladder", {"Speed", "bAllowStrafe", nullptr}},
    {"WaterCurrent", {"fSpeed", "fAcceleration", "bVehicleOnly"}},
    {"ProximityDamage", {"fDamageRate", "selDamageType", "bInstantDamage"}},
    {"PlaceableExplo", {"PlaySequence", nullptr, nullptr}},
    {"AutomaticDoor", {"MovingDistance", "CloseDelay", "bPlayerOnly"}},
    {"AutomaticDoor1Piece", {"MovingDistance", "CloseDelay", "bPlayerOnly"}},
};
constexpr const char* kMountedWeapons[] = {"MountedWeaponBrowning", "MountedWeaponMortar"};
constexpr const char* kPickups[] = {"Health", "Health_Large", "Armor", "ArmorHigh", "MRE", "Secret"};
constexpr const char* kPickupPrefix = "Pickup";
constexpr const char* kOtherClasses[] = {"ZipLine", "DamageArea", "DamagingAnimation", "Waypoint"};

constexpr ImU32 kGameplayColour = IM_COL32(120, 230, 160, 255);
constexpr ImU32 kDamageColour = IM_COL32(255, 90, 40, 255);
constexpr ImU32 kObjectiveColour = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kPickupColour = IM_COL32(170, 230, 255, 255);
constexpr ImU32 kSecretColour = IM_COL32(255, 215, 0, 255);

template <size_t N>
bool IsOneOf(const std::string& text, const char* const (&list)[N]) {
    for (const char* item : list) {
        if (text == item) return true;
    }
    return false;
}

const BoxClass* BoxClassOf(const std::string& cls) {
    for (const BoxClass& b : kBoxClasses) {
        if (cls == b.cls) return &b;
    }
    return nullptr;
}

bool IsPickup(const std::string& cls) { return cls.rfind(kPickupPrefix, 0) == 0 || IsOneOf(cls, kPickups); }

float FloatOf(const ViewEntity& e, const char* key) { return std::strtof(PropertyOf(e, key).c_str(), nullptr); }

void Centre(const ViewEntity& e, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (e.box_min[i] + e.box_max[i]) * 0.5f;
}

void LabelNear(const View& v, const float at[3], const std::string& text, ImU32 colour) {
    if (NearCamera(v, at)) Label(v, at, text.c_str(), colour);
}

// the current's push for one second
void DrawCurrent(const View& v, const ViewEntity& e) {
    float dir[3] = {};
    if (PropertyOf(e, "bUseEntityOrientation") == "1") {
        float axes[3][3];
        AxesFromAngles(e.angles, axes);
        for (int i = 0; i < 3; i++) dir[i] = axes[1][i];
    } else {
        std::string text = PropertyOf(e, "vector_Direction");
        std::sscanf(text.c_str(), "%f,%f,%f", &dir[0], &dir[1], &dir[2]);
    }
    float from[3];
    Centre(e, from);
    float speed = FloatOf(e, "fSpeed");
    float to[3];
    for (int i = 0; i < 3; i++) to[i] = from[i] + dir[i] * speed;
    Arrow(v, from, to, kGameplayColour);
}

void DrawBoxed(const View& v, const ViewEntity& e, const BoxClass& b) {
    bool damage = e.cls == "ProximityDamage" || e.cls == "PlaceableExplo";
    ImU32 colour = damage ? kDamageColour : kGameplayColour;
    if (e.has_box) AxisBox(v, e.box_min, e.box_max, colour);
    else Square(v, e.pos, colour);
    if (e.cls == "WaterCurrent" && e.has_box) DrawCurrent(v, e);
    std::string text = e.name + " (" + e.cls + ")";
    for (const char* key : b.shown) {
        if (key && !PropertyOf(e, key).empty()) text += "\n" + std::string(key) + " " + PropertyOf(e, key);
    }
    LabelNear(v, e.pos, text, colour);
}

void DrawZipLine(const View& v, const ViewEntity& e, const std::unordered_map<std::string, const AiObject*>& tags) {
    auto from = tags.find(PropertyOf(e, "fromTagPoint"));
    auto to = tags.find(PropertyOf(e, "toTagPoint"));
    Square(v, e.pos, kGameplayColour);
    if (from == tags.end() || to == tags.end()) return;
    Arrow(v, from->second->pos, to->second->pos, kGameplayColour);
    LabelNear(v, from->second->pos, e.name + " (ZipLine)\nmaxSpeed " + PropertyOf(e, "maxSpeed"), kGameplayColour);
}

void DrawWaypoint(const View& v, const ViewEntity& e) {
    if (e.has_box) AxisBox(v, e.box_min, e.box_max, kObjectiveColour);
    else Circle(v, e.pos, FloatOf(e, "Radius"), kObjectiveColour);
    Square(v, e.pos, kObjectiveColour);
    std::string text = e.name + " (objective " + PropertyOf(e, "TableIndex") + ", status " +
                       PropertyOf(e, "selStatus") + ")\nWaypointTag " + PropertyOf(e, "WaypointTag") +
                       "\nCompletionTag " + PropertyOf(e, "CompletionTag");
    LabelNear(v, e.pos, text, kObjectiveColour);
}

void DrawOther(const View& v, const ViewEntity& e, const std::unordered_map<std::string, const AiObject*>& tags) {
    if (e.cls == "ZipLine") return DrawZipLine(v, e, tags);
    if (e.cls == "Waypoint") return DrawWaypoint(v, e);
    Square(v, e.pos, kDamageColour);
    if (e.cls == "DamageArea") {
        DrawEntityAreas(v, e.name, e.pos, kDamageColour);
        LabelNear(v, e.pos, e.name + " (DamageArea)\ndamageRate " + PropertyOf(e, "damageRate"), kDamageColour);
        return;
    }
    Circle(v, e.pos, FloatOf(e, "Radius"), kDamageColour);
    LabelNear(v, e.pos, e.name + " (DamagingAnimation)\nDamage " + PropertyOf(e, "Damage"), kDamageColour);
}

void DrawPickup(const View& v, const ViewEntity& e) {
    bool secret = e.cls == "Secret";
    ImU32 colour = secret ? kSecretColour : kPickupColour;
    Square(v, e.pos, colour);
    std::string text = e.name + " (" + e.cls + ")";
    if (secret) text += "\nSecretID " + PropertyOf(e, "SecretID");
    else if (!PropertyOf(e, "Amount").empty()) text += "\nAmount " + PropertyOf(e, "Amount");
    LabelNear(v, e.pos, text, colour);
}

void DrawMountedWeapon(const View& v, const ViewEntity& e) {
    Square(v, e.pos, kDamageColour);
    std::string text = e.name + " (" + e.cls + ")\nangHLimit " + PropertyOf(e, "angHLimit") + "\nangVLimit " +
                       PropertyOf(e, "angVLimitMin") + " to " + PropertyOf(e, "angVLimitMax");
    LabelNear(v, e.pos, text, kDamageColour);
}

}  // namespace

bool fcip::editor::IsGameplay(const std::string& cls) {
    return BoxClassOf(cls) || IsOneOf(cls, kMountedWeapons) || IsOneOf(cls, kOtherClasses) || IsPickup(cls);
}

void fcip::editor::DrawGameplay(const View& v) {
    std::unordered_map<std::string, const AiObject*> tags;
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    for (const AiObject& o : ai->objects) {
        if (o.type == "TagPoint" && o.placed) tags[o.name] = &o;
    }
    for (const ViewEntity& e : *ViewEntities()) {
        if (const BoxClass* b = BoxClassOf(e.cls)) DrawBoxed(v, e, *b);
        else if (IsOneOf(e.cls, kMountedWeapons)) DrawMountedWeapon(v, e);
        else if (IsOneOf(e.cls, kOtherClasses)) DrawOther(v, e, tags);
        else if (IsPickup(e.cls)) DrawPickup(v, e);
    }
}
