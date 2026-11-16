// what the solo editor places, grouped as the editor view groups them (markers.h); the dock lists them by category
#pragma once

#include "leveleditor/markers.h"

namespace fcip::editor {

constexpr int kPickupsCategory = -1;  // stays with the weapons and items

// an entity class the Add Object tool spawns with its script's defaults, from the catalogue's weapons and items
struct LevelClass {
    const char* cls;
    const char* caption;
    int category;
};
inline constexpr LevelClass kLevelClasses[] = {
    {"PickupAdrenaline", "Adrenaline", kPickupsCategory},
    {"MRE", "Ration Pack", kPickupsCategory},
    {"PickupGrenadeBox", "Grenade Box", kPickupsCategory},
    {"PickupMolotovBox", "Molotov Box", kPickupsCategory},
    {"PickupBerrettaNoSup", "9mm Pistol (No Silencer)", kPickupsCategory},
    {"PickupBinoculars", "Binoculars", kPickupsCategory},
    {"PickupExplo", "Explosives", kPickupsCategory},
    {"PickupMortar", "Mortar", kPickupsCategory},
    {"PickupBrowning", "Heavy Machine Gun", kPickupsCategory},
    {"Checkpoint", "Checkpoint", kViewPlayerStart},
    {"EndMapTrigger", "Level End", kViewPlayerStart},
    {"ProximityTrigger", "Proximity Trigger", kViewTriggers},
    {"AreaTrigger", "Area Trigger", kViewTriggers},
    {"DelayTrigger", "Delay Trigger", kViewTriggers},
    {"MultipleTrigger", "Multiple Trigger", kViewTriggers},
    {"UtilityTrigger", "Utility Trigger", kViewTriggers},
    {"EnvironmentTrigger", "Environment Trigger", kViewTriggers},
    {"BlinkingTrigger", "Blinking Trigger", kViewTriggers},
    {"ViewDist", "View Distance", kViewCulling},
    {"SoundSpot", "Sound Spot", kViewSounds},
    {"ProximitySoundSpot", "Proximity Sound Spot", kViewSounds},
    {"AreaSound", "Area Sound", kViewSounds},
    {"AreaSoundTriggerKit", "Area Sound Trigger Kit", kViewSounds},
    {"SoundSupressor", "Sound Suppressor", kViewSounds},
    {"CrazyFollowerSound", "Follower Sound", kViewSounds},
    {"MicroFollowerSound", "Micro Follower Sound", kViewSounds},
    {"DynamicLight", "Dynamic Light", kViewLights},
    {"ParticleEffect", "Particle Effect", kViewLights},
    {"Ladder", "Ladder", kViewGameplay},
    {"WaterCurrent", "Water Current", kViewGameplay},
    {"ProximityDamage", "Proximity Damage", kViewGameplay},
    {"DamageArea", "Damage Area", kViewGameplay},
    {"DamagingAnimation", "Damaging Animation", kViewGameplay},
    {"Waypoint", "Objective Waypoint", kViewGameplay},
    {"Secret", "Secret", kViewGameplay},
    {"VSAreaModifier", "Area Modifier", kViewGameplay},
    {"SonicAlarm", "Sonic Alarm", kViewGameplay},
    {"SonicAlarmEmitter", "Sonic Alarm Emitter", kViewGameplay},
    {"BeaconEmitter", "Beacon Emitter", kViewGameplay},
    {"PlaceableExplo", "Placed Explosive", kViewGameplay},
    {"Swapper", "Swapper", kViewGameplay},
    {"Birds", "Birds", kViewGameplay},
    {"Fish", "Fish", kViewGameplay},
    {"DeadBody", "Dead Body", kViewGameplay},
    {"ZipLine", "Zip Line", kViewGameplay},
    {"MountedWeaponBrowning", "Mounted Machine Gun", kViewGameplay},
    {"MountedWeaponMortar", "Mounted Mortar", kViewGameplay},
    {"Claymore", "Claymore", kViewGameplay},
    {"BranchWhip", "Branch Whip Trap", kViewGameplay},
    {"CreatureGenerator", "Creature Generator", kViewAi},
    {"CreatureGeneratorSpawnSpot", "Generator Spawn Spot", kViewAi},
};

// an <Object> of the kept mission, put at the tool cursor
struct LevelObject {
    const char* type;
    const char* caption;
    int category;
};
inline constexpr LevelObject kLevelObjects[] = {
    {"Respawn", "Respawn Point", kViewPlayerStart},
    {"AreaBox", "Area Box", kViewTriggers},
    {"AreaSphere", "Area Sphere", kViewTriggers},
    {"Shape", "Area Shape", kViewTriggers},
    {"VisArea", "Vis Area", kViewCulling},
    {"Portal", "Portal", kViewCulling},
    {"AIAnchor", "AI Anchor", kViewAi},
    {"TagPoint", "Tag Point", kViewAi},
    {"AIPath", "AI Path", kViewAi},
    {"ForbiddenArea", "Forbidden Area", kViewAi},
    {"EnclosingArea", "Enclosing Area", kViewAi},
    {"FenceArea", "Fence Area", kViewAi},
    {"AINavigationModifier", "Navigation Modifier", kViewAi},
};

void DrawLevelObjects();  // window thread, the dock's Objects tab (object_browser.cpp)

}  // namespace fcip::editor
