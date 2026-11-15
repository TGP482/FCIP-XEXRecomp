// what each entity class sends and takes as events, from its script's Event_ functions (lua.dat) and the campaign
// missions' EventTargets; and the mission's own calls (the Mission table's Event_ functions), for the link lists
#pragma once
#include <string>
#include <vector>

namespace fcip::editor {

// a class and its events, a space between each
struct ClassEvents {
    const char* cls;
    const char* events;
};
inline constexpr ClassEvents kClassEvents[] = {
    {"AreaSound", "ChangeMultiTrackVolume Disable Enable Play Spawn Stop"},
    {"AreaSoundTriggerKit", "Disable Enable Enter Leave Spawn"},
    {"AreaTrigger", "Disable Enable Enter Leave Spawn"},
    {"AutomaticDoor", "Activate Close Closed Deactivate Open Opened Unlocked"},
    {"AutomaticDoor1Piece", "Close Closed Disable Enable Open Opened"},
    {"AutomaticElevator", "Activate Close Closed Deactivate Open Opened"},
    {"BasicEntity", "Activate AddImpulse Hide StartBink UnHide"},
    {"BeaconEmitter", "Disable Enable Play Stop"},
    {"BeaconNoiseEmitter", "Disable Enable"},
    {"Birds", "Activate Deactivate"},
    {"BlinkingTrigger", "Blinking Switch"},
    {"Boat", "AddPlayer BlackHole EveryoneOut GoAttack GoChaseBoat GoCircle GoPath GoPathDone GoPathLoopDone "
             "GoPatrol GoTrailBoat IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
             "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited KillTriger Load "
             "PauseDynamicAvoidance Reinforcment ReinforcmentDone Release StartFire StopFire TArgetOnLand "
             "UnBindObject UnPauseDynamicAvoidance WaitPlayer"},
    {"BranchWhip", "Disable Enable EveryoneOut Timer Trigger"},
    {"Buggy", "BlackHole DamageTriger Disable Enable EveryoneOut GoChase GoChaseCar GoCircle GoPath GoPathDone "
              "GoPathLoopDone GoPatrol GoPoint GoPointDone IO_Destroyed IO_DriverEntered IO_DriverExited "
              "IO_GunnerEntered IO_GunnerExited IO_PassengerEntered IO_PassengerExited IO_PlayerEntered "
              "IO_PlayerExited KillTriger LoadPeople PauseDynamicAvoidance Reinforcment UnPauseDynamicAvoidance "
              "WaitPlayer"},
    {"Checkpoint", "Disable Enable Enter Trigger"},
    {"Chucker", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
                "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
                "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
                "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
                "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath "
                "OnMakeInVulnerable OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle "
                "Output_AlertnessInterested Output_AlertnessThreatened Output_OnCorpseSeen "
                "Output_RadioHelpRequested PickupGenericPickUp PlaySound RebelLeaderDied RebelLeader_Attacked "
                "ReinforcmentDone Relocate Reset Stage Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset "
                "Stage3Destroyed Stage3Reset StartBink StartCombat StartCroweFight StartFeralLeaderFight StopBink "
                "StopCombat SwitchToGenericJob Trigger UnHide"},
    {"Claymore", "Trigger"},
    {"Cloth", "Activate Hide UnHide"},
    {"CrazyFollowerSound", "Off On"},
    {"CreatureGenerator", "Creature CreatureActivate CreatureAlmostDead CreatureDeActivate CreatureDie "
                          "CreatureDoYourJob CreatureLastGroupMemberDied CreatureMakeInvulnerable "
                          "CreatureMakeVulnerable CreatureOnDeath CreatureOutput_AlertnessAttack "
                          "CreatureOutput_AlertnessIdle CreatureOutput_AlertnessInterested "
                          "CreatureOutput_AlertnessThreatened CreaturePlaySound CreatureRelocate "
                          "CreatureSwitchToGenericJob Disable Enable KillEmAll OnLastSpawnedEntityDied "
                          "OnLastSpawnedEntitySpawned OnSpawnedEntityDied OnSpawnedEntitySpawned Spawn SpawnMax "
                          "Switch"},
    {"CreatureGeneratorSpawnSpot", "CreatureActivate CreatureAlmostDead CreatureDeActivate CreatureDie "
                                   "CreatureDoYourJob CreatureLastGroupMemberDied CreatureMakeInvulnerable "
                                   "CreatureMakeVulnerable CreatureOnDeath CreatureOutput_AlertnessAttack "
                                   "CreatureOutput_AlertnessIdle CreatureOutput_AlertnessInterested "
                                   "CreatureOutput_AlertnessThreatened CreaturePlaySound CreatureRelocate "
                                   "CreatureSwitchToGenericJob Disable Enable Output_Spawn Switch"},
    {"Crowe", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
              "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
              "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
              "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
              "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath OnMakeInVulnerable "
              "OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle Output_AlertnessInterested "
              "Output_AlertnessThreatened Output_OnCorpseSeen Output_RadioHelpRequested PickupGenericPickUp "
              "PlaySound RebelLeaderDied RebelLeader_Attacked ReinforcmentDone Relocate Reset Stage "
              "Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset Stage3Destroyed Stage3Reset StartBink "
              "StartCombat StartCroweFight StartFeralLeaderFight StopBink StopCombat SwitchToGenericJob Trigger "
              "UnHide"},
    {"DamageArea", "Disable Enable Switch"},
    {"DamagingAnimation", "Switch"},
    {"DeadBody", "Awake"},
    {"DelayTrigger", "Disable Enable InputTrigger OutputTrigger"},
    {"DestroyableObject", "Explode OnDamage OnMakeInVulnerable OnMakeVulnerable Reset Stage1Destroyed "
                          "Stage2Destroyed Stage3Destroyed UnHide"},
    {"Dragon", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
               "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
               "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
               "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
               "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath "
               "OnMakeInVulnerable OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle "
               "Output_AlertnessInterested Output_AlertnessThreatened Output_OnCorpseSeen "
               "Output_RadioHelpRequested PickupGenericPickUp PlaySound RebelLeaderDied RebelLeader_Attacked "
               "ReinforcmentDone Relocate Reset Stage Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset "
               "Stage3Destroyed Stage3Reset StartBink StartCombat StartCroweFight StartFeralLeaderFight StopBink "
               "StopCombat SwitchToGenericJob Trigger UnHide"},
    {"DynamicLight", "Disable Enable Shake"},
    {"EndMapTrigger", "Disable Enable Enter Trigger"},
    {"EnvironmentTrigger", "ApplyFog ApplyGlow Disable Enable Enter Leave"},
    {"Fish", "Activate Deactivate"},
    {"Freak", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
              "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
              "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
              "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
              "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath OnMakeInVulnerable "
              "OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle Output_AlertnessInterested "
              "Output_AlertnessThreatened Output_OnCorpseSeen Output_RadioHelpRequested PickupGenericPickUp "
              "PlaySound RebelLeaderDied RebelLeader_Attacked ReinforcmentDone Relocate Reset Stage "
              "Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset Stage3Destroyed Stage3Reset StartBink "
              "StartCombat StartCroweFight StartFeralLeaderFight StopBink StopCombat SwitchToGenericJob Trigger "
              "UnHide"},
    {"Gunship", "BlackHole Enable FadeInSound FadeOutSound Fly GoAttack GoAttackPoint GoPath GoPathDone "
                "GoPathLoopDone GoPatrol Kill KillCarver Land LoadPeople LowHealth Reinforcment ReinforcmentDone "
                "ReinforcmentRappel StartFire StopFire Unhide"},
    {"Hovercraft", "IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
                   "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited KillTriger"},
    {"Humvee", "AIDriverIn AIDriverOut BlackHole DamageTriger Disable Enable EveryoneOut FadeInSound FadeOutSound "
               "GoChase GoChaseCar GoCircle GoPath GoPathDone GoPathLoopDone GoPatrol GoPoint GoPointDone "
               "IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
               "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited "
               "Input_RailshootingSequenceDone KillTriger LoadPeople MakePlayerGunner PauseDynamicAvoidance "
               "PlayerEntered Reinforcment SuperCharged SuperChargedEnd UnPauseDynamicAvoidance WaitPlayer Wakeup"},
    {"JetSki", "AddPlayer BlackHole EveryoneOut GoChaseBoat GoCircle GoPath GoPathDone GoPathLoopDone GoTrailBoat "
               "IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
               "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited KillJetSki LoadPeople "
               "PauseDynamicAvoidance ReinforcmentDone StartAniPath UnPauseDynamicAvoidance WaitPlayer"},
    {"Ladder", "Disable Enable"},
    {"Merc", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
             "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
             "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
             "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
             "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath OnMakeInVulnerable "
             "OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle Output_AlertnessInterested "
             "Output_AlertnessThreatened Output_OnCorpseSeen Output_RadioHelpRequested PickupGenericPickUp "
             "PlaySound PlayerSeen RebelLeaderDied RebelLeader_Attacked ReinforcmentDone Relocate Reset Stage "
             "Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset Stage3Destroyed Stage3Reset StartBink "
             "StartCombat StartCroweFight StartFeralLeaderFight StopBink StopCombat SwitchToGenericJob Trigger "
             "UnHide"},
    {"MicroFollowerSound", "Off On"},
    {"MountedWeaponBrowning", "OnUse StopUse Use"},
    {"MountedWeaponMortar", "StopUse Use"},
    {"MultipleTrigger", "Disable Enable InputTrigger OutputTrigger"},
    {"Paraglider", "IO_Destroyed IO_PlayerEntered IO_PlayerExited"},
    {"ParticleEffect", "Disable Enable Pulse Switch"},
    {"PickupBerrettaNoSup", "Picked"},
    {"PickupBlowGun", "Picked"},
    {"PickupClaymore", "Picked"},
    {"PickupExplo", "Picked"},
    {"PickupGrenadeBox", "Picked"},
    {"PickupHKSL8", "Picked"},
    {"PickupPipeBomb", "Picked"},
    {"PickupTruck", "AIDriverIn AIDriverOut BlackHole DamageTriger Disable Enable EveryoneOut FadeInSound "
                    "FadeOutSound GoChase GoChaseCar GoCircle GoPath GoPathDone GoPathLoopDone GoPatrol GoPoint "
                    "GoPointDone IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
                    "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited "
                    "Input_RailshootingSequenceDone KillTriger LoadPeople MakePlayerGunner PauseDynamicAvoidance "
                    "Reinforcment SuperCharged SuperChargedEnd UnPauseDynamicAvoidance WaitPlayer Wakeup"},
    {"PlaceableExplo", "Activate DeActivateAndHide Explode Hide Show"},
    {"PredatorStationLights", "Disable Enable"},
    {"ProximityDamage", "Disable Enable Switch"},
    {"ProximitySoundSpot", "Disable Enable Stop"},
    {"ProximityTrigger", "Disable DisplayText Enable Enter Leave"},
    {"ProximityTrigger_MP", "Disable Enable Enter Leave OnEvolution OnStance OnUse SwitchOff SwitchOn"},
    {"Sampan", "AddPlayer BlackHole EveryoneOut GoAttack GoChaseBoat GoCircle GoPath GoPathDone GoPathLoopDone "
               "GoPatrol IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
               "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited KillTriger Load "
               "PauseDynamicAvoidance Reinforcment ReinforcmentDone Release StartFire StopFire TArgetOnLand "
               "UnBindObject UnPauseDynamicAvoidance WaitPlayer"},
    {"Sapper", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
               "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
               "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
               "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
               "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath "
               "OnMakeInVulnerable OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle "
               "Output_AlertnessInterested Output_AlertnessThreatened Output_OnCorpseSeen "
               "Output_RadioHelpRequested PickupGenericPickUp PlaySound RebelLeaderDied RebelLeader_Attacked "
               "ReinforcmentDone Relocate Reset Stage Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset "
               "Stage3Destroyed Stage3Reset StartBink StartCombat StartCroweFight StartFeralLeaderFight StopBink "
               "StopCombat SwitchToGenericJob Trigger UnHide"},
    {"SonicAlarm", "AlarmDestroy AlarmTurnOff AlarmTurnOn OnDamage"},
    {"SoundSpot", "Disable Enable Play Stop"},
    {"SoundSupressor", "Disable Enable"},
    {"StaticVehicle", "Activate DeActivate Enter IO_Destroyed IO_PlayerEntered IO_PlayerExited"},
    {"Swapper", "SwitchModel SwitchOnOff"},
    {"Swinging", "Disable Enable EveryoneOut Timer Trigger"},
    {"Unimog", "AIDriverIn AIDriverOut BlackHole DamageTriger Disable Enable EveryoneOut FadeInSound FadeOutSound "
               "GoChase GoChaseCar GoPath GoPathDone GoPathLoopDone GoPatrol GoPoint GoPointDone IO_Destroyed "
               "IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited IO_PassengerEntered "
               "IO_PassengerExited IO_PlayerEntered IO_PlayerExited KillTriger LoadPeople MakePlayerGunner "
               "PauseDynamicAvoidance Reinforcment SuperCharged SuperChargedEnd UnPauseDynamicAvoidance "
               "WaitPlayer Wakeup"},
    {"UtilityTrigger", "Disable Enable Enter Leave StartLooking StopLooking SwitchSequence"},
    {"VSAreaModifier", "ActivateArea ChangeVolume DeactivateArea Disable Enable ModifyDynParams ResetDynParams"},
    {"Valerie", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
                "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
                "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
                "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
                "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath "
                "OnMakeInVulnerable OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle "
                "Output_AlertnessInterested Output_AlertnessThreatened Output_OnCorpseSeen "
                "Output_RadioHelpRequested PickupGenericPickUp PlaySound RebelLeaderDied RebelLeader_Attacked "
                "ReinforcmentDone Relocate Reset Stage Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset "
                "Stage3Destroyed Stage3Reset StartBink StartCombat StartCroweFight StartFeralLeaderFight StopBink "
                "StopCombat SwitchToGenericJob Trigger UnHide"},
    {"WaterCurrent", "Activate ChangeAcceleration ChangeDirection ChangeSpeed Deactivate"},
    {"Waypoint", "BlinkOff BlinkOn Enter SetAbsent SetClosed SetCurrent SetOpen Trigger"},
    {"Worker", "Activate AddImpulse AlmostDead Ambush_AttackPlayer BossFightIntro ChangeJumpMinSearchHeight "
               "CroweSixthSenseTriggered DeActivate Die Disable DisableCroweSixthSense DisableObject DoYourJob "
               "Enable EnableCroweSixthSense EndBossFight Explode GetSecondBrowning GiveImpulse GoPathDone "
               "GoPathLoopDone GoPointDone Hide InitBossFight InjectCarver IsStartingStage2 IsStartingStage3 "
               "KillTriger LastGroupMemberDied MakeInvulnerable MakeVulnerable OnDamage OnDeath "
               "OnMakeInVulnerable OnMakeVulnerable Output_AlertnessAttack Output_AlertnessIdle "
               "Output_AlertnessInterested Output_AlertnessThreatened Output_OnCorpseSeen "
               "Output_RadioHelpRequested PickupGenericPickUp PlaySound RebelLeaderDied RebelLeader_Attacked "
               "ReinforcmentDone Relocate Reset Stage Stage1Destroyed Stage1Reset Stage2Destroyed Stage2Reset "
               "Stage3Destroyed Stage3Reset StartBink StartCombat StartCroweFight StartFeralLeaderFight StopBink "
               "StopCombat SwitchToGenericJob Trigger UnHide"},
    {"Zodiac", "EveryoneOut IO_Destroyed IO_DriverEntered IO_DriverExited IO_GunnerEntered IO_GunnerExited "
               "IO_PassengerEntered IO_PassengerExited IO_PlayerEntered IO_PlayerExited"},
};

// what a mission call's ExtraParam names
enum class CallParam { None, Sequence, Anchor, EntityAndBone, Names, Text };
struct MissionCall {
    const char* event;
    CallParam param;
    const char* usual;  // the campaign's values, a space between each
};
inline constexpr MissionCall kMissionCalls[] = {
    {"_Mission_PlaySequence", CallParam::Sequence, ""},
    {"_Mission_StopSequence", CallParam::Sequence, ""},
    {"_Mission_Failed", CallParam::None, ""},
    {"_GUI_BeginCutScene", CallParam::Text, "-,look_only"},
    {"_GUI_EndCutScene", CallParam::None, ""},
    {"_GUI_HideHUD", CallParam::Text, "0.1 2"},
    {"_GUI_ShowHUD", CallParam::Text, "2"},
    {"_GUI_RadarBlip", CallParam::Names, ""},
    {"_GenericJob_EnableAnchor", CallParam::Anchor, ""},
    {"_GenericJob_DisableAnchor", CallParam::Anchor, ""},
    {"_Player_BindEyes", CallParam::EntityAndBone, ""},
    {"_Player_UnbindEyes", CallParam::None, ""},
    {"_Player_DoAction", CallParam::Text,
     "AdrenalineMaxOff AdrenalineMaxOn Evol-Athletic-Off Evol-Athletic-On Evol-MotionTracker-Avail-Off "
     "Evol-MotionTracker-Avail-On Evol-NightVision-Avail-Off Evol-NightVision-Avail-On Evol-Visions-Off "
     "Evol-Visions-On Flush-Inventory Flush-Inventory-Partial interaction"},
    {"_Player_PlayAnimation", CallParam::Text, ""},
    {"_Player_PlayAnimationWithActionMap", CallParam::Text, ""},
    {"_Player_SetActionMap", CallParam::Text, "look_only move_only RIV_NoPunch"},
    {"_Player_RestoreActionMap", CallParam::None, ""},
};

// the words of a list a space apart
inline std::vector<std::string> Words(const std::string& list) {
    std::vector<std::string> words;
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find(' ', at);
        if (end == std::string::npos) end = list.size();
        words.push_back(list.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

// the class's events, none for a class the table lacks
inline std::vector<std::string> EventsOf(const std::string& cls) {
    for (const ClassEvents& c : kClassEvents) {
        if (cls == c.cls) return Words(c.events);
    }
    return {};
}

inline const MissionCall* MissionCallOf(const std::string& event) {
    for (const MissionCall& c : kMissionCalls) {
        if (event == c.event) return &c;
    }
    return nullptr;
}

}  // namespace fcip::editor
