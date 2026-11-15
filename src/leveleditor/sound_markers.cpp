// ige.xex, the solo level editor: sounds, lights and effects, as their scripts (lua.dat) use their Properties. A
// SoundSpot's emitter plays from where it stands; its InnerRadius and OuterRadius are not read by its script, the
// fRollOff distances are, when not -1 (the sound event's own otherwise). A ProximitySoundSpot plays while the player is
// in the box SetLocalBBox gives it from OuterRadius. AreaSound and AreaSoundTriggerKit play from the areas naming
// them. SoundSupressor and SonicAlarm reach fRadius and DamageRadius. A DynamicLight lights AttenuationRadius around
// it (SetRadius) in clrDiffuse; a ParticleEffect spawns its ParticlesSystem effect where it stands.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "leveleditor/markers.h"

using namespace fcip::editor;

namespace {

constexpr const char* kSoundClasses[] = {"SoundSpot",      "ProximitySoundSpot", "AreaSound",
                                         "AreaSoundTriggerKit", "CrazyFollowerSound", "SoundSupressor",
                                         "BeaconEmitter",  "SonicAlarm",         "SonicAlarmEmitter"};
constexpr const char* kLightClasses[] = {"DynamicLight", "ParticleEffect"};
// SoundSpot's distances, in its script's order
constexpr const char* kRollOffs[] = {"fRollOff1_Saturation", "fRollOff2_Background", "fRollOff3_Transition",
                                     "fRollOff4_Inaudible"};

constexpr ImU32 kSoundColour = IM_COL32(100, 170, 255, 255);
constexpr ImU32 kSoundRangeColour = IM_COL32(100, 170, 255, 110);
constexpr ImU32 kEffectColour = IM_COL32(255, 130, 170, 255);
constexpr int kAlpha = 200;

float FloatOf(const ViewEntity& e, const char* key, float fallback) {
    std::string text = PropertyOf(e, key);
    return text.empty() ? fallback : std::strtof(text.c_str(), nullptr);
}

template <size_t N>
bool IsOneOf(const std::string& text, const char* const (&list)[N]) {
    for (const char* item : list) {
        if (text == item) return true;
    }
    return false;
}

std::string SoundEvents(const ViewEntity& e) {
    std::string text;
    for (const XmlAttribute& a : e.properties) {
        bool sound = a.key.rfind("snd", 0) == 0;
        if (sound && a.value != "-1" && !a.value.empty()) text += "\n" + a.key + " " + a.value;
    }
    return text;
}

void DrawSound(const View& v, const ViewEntity& e, const std::unordered_map<std::string, const ViewEntity*>& names) {
    Square(v, e.pos, kSoundColour);
    if (e.cls == "SoundSpot") {
        for (const char* key : kRollOffs) {
            float distance = FloatOf(e, key, -1.0f);
            if (distance > 0.0f) Circle(v, e.pos, distance, kSoundRangeColour);
        }
    }
    if (e.cls == "ProximitySoundSpot" && e.has_box) AxisBox(v, e.box_min, e.box_max, kSoundRangeColour);
    if (e.cls == "AreaSound" || e.cls == "AreaSoundTriggerKit") DrawEntityAreas(v, e.name, e.pos, kSoundRangeColour);
    if (e.cls == "SoundSupressor") Circle(v, e.pos, FloatOf(e, "fRadius", 0.0f), kSoundRangeColour);
    if (e.cls == "SonicAlarm") {
        Circle(v, e.pos, FloatOf(e, "DamageRadius", 0.0f), kSoundRangeColour);
        auto emitter = names.find(PropertyOf(e, "UltraSoundEmitterName"));
        if (emitter != names.end()) Arrow(v, e.pos, emitter->second->pos, kSoundColour);
    }
    if (!NearCamera(v, e.pos)) return;
    std::string text = e.name + " (" + e.cls + ")";
    if (PropertyOf(e, "bEnabled") == "0") text += "\ndisabled";
    Label(v, e.pos, (text + SoundEvents(e)).c_str(), kSoundColour);
}

// clrDiffuse is "r,g,b" from 0 to 1
ImU32 LightColour(const ViewEntity& e) {
    float rgb[3] = {1.0f, 1.0f, 1.0f};
    std::string text = PropertyOf(e, "clrDiffuse");
    std::sscanf(text.c_str(), "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]);
    auto channel = [](float c) { return int(std::clamp(c, 0.0f, 1.0f) * 255.0f); };
    return IM_COL32(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]), kAlpha);
}

void DrawLight(const View& v, const ViewEntity& e) {
    ImU32 colour = LightColour(e);
    Square(v, e.pos, colour);
    Circle(v, e.pos, FloatOf(e, "AttenuationRadius", 0.0f), colour);
    if (!NearCamera(v, e.pos)) return;
    std::string text = e.name + " (DynamicLight " + PropertyOf(e, "enumLightType") + ")\nAttenuationRadius " +
                       PropertyOf(e, "AttenuationRadius");
    if (PropertyOf(e, "bActive") == "0") text += "\noff";
    Label(v, e.pos, text.c_str(), colour);
}

void DrawEffect(const View& v, const ViewEntity& e) {
    Square(v, e.pos, kEffectColour);
    if (!NearCamera(v, e.pos)) return;
    std::string text = e.name + "\n" + PropertyOf(e, "ParticlesSystem/effectParticleEffect");
    if (PropertyOf(e, "bActive") == "0") text += "\noff";
    Label(v, e.pos, text.c_str(), kEffectColour);
}

}  // namespace

bool fcip::editor::IsSoundOrLight(const std::string& cls) {
    return IsOneOf(cls, kSoundClasses) || IsOneOf(cls, kLightClasses);
}

void fcip::editor::DrawSounds(const View& v) {
    std::shared_ptr<const std::vector<ViewEntity>> entities = ViewEntities();
    std::unordered_map<std::string, const ViewEntity*> names;
    for (const ViewEntity& e : *entities) names[e.name] = &e;
    for (const ViewEntity& e : *entities) {
        if (IsOneOf(e.cls, kSoundClasses)) DrawSound(v, e, names);
    }
}

void fcip::editor::DrawLights(const View& v) {
    for (const ViewEntity& e : *ViewEntities()) {
        if (e.cls == "DynamicLight") DrawLight(v, e);
        if (e.cls == "ParticleEffect") DrawEffect(v, e);
    }
}
