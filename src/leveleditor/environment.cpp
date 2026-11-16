// ige.xex, the solo level editor: the Environment tab edits the kept mission's <Environment> (fog, sky, sun, ocean,
// glow, rain and the rest) and has the engine load it again, as the map editor's time of day presets do (820E0240
// runs the loader 82361DB0 on a preset). The fields are the keys that loader reads, with the type it reads them as;
// colours are "r,g,b" from 0 to 255. The sun's direction is SunVector (and SunVectorRender for drawing), which the
// campaign's files keep beside the rotation and height they were made from: x = sin(rotation) * h,
// y = cos(rotation) * h, z = -height / 100, h = sqrt(1 - z * z). The tab edits rotation and height and writes all of
// them. Heat, EnvColor and the PC editor's other Lighting keys are not read by the game and are left as they are.
#include "leveleditor/environment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <span>
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

constexpr const char* kEnvironment = "Environment";
constexpr const char* kLighting = "Lighting";

enum class Kind { Bool, Int, Float, Colour, Vector2, Texture, Effect };
struct Field {
    const char* node;
    const char* key;
    Kind kind;
};
// what 82361DB0 reads, in its order
constexpr Field kFields[] = {
    {"Fog", "Color", Kind::Colour},
    {"Fog", "Start", Kind::Float},
    {"Fog", "End", Kind::Float},
    {"Fog", "FogAmount", Kind::Float},
    {"Camera", "ViewDistance", Kind::Float},
    {"Camera", "DetailViewDistance", Kind::Float},
    {"Glow", "Enable", Kind::Bool},
    {"Glow", "Brightness", Kind::Int},
    {"Glow", "Intensity", Kind::Float},
    {"Glow", "Bias", Kind::Float},
    {"SkyBox", "SkyBox_12", Kind::Texture},
    {"SkyBox", "SkyBox_34", Kind::Texture},
    {"SkyBox", "SkyBox_5", Kind::Texture},
    {"SkyBox", "Sun", Kind::Texture},
    {"SkyBox", "SunSize", Kind::Float},
    {"SkyBox", "Flare", Kind::Texture},
    {"SkyBox", "FlareSize", Kind::Float},
    {"SkyBox", "RotationSpeed", Kind::Float},
    {"DetailGrass", "TiledTexture", Kind::Texture},
    {"DetailGrass", "ViewDistance", Kind::Float},
    {"DetailGrass", "WidthMin", Kind::Float},
    {"DetailGrass", "WidthMax", Kind::Float},
    {"DetailGrass", "HeightMin", Kind::Float},
    {"DetailGrass", "HeightMax", Kind::Float},
    {"Vegetation", "TwoSidedLightingTransmitPercent", Kind::Float},
    {"EnvState", "SunColor", Kind::Colour},
    {"EnvState", "SunLuminosity", Kind::Float},
    {"EnvState", "SunBlindingEnable", Kind::Bool},
    {"EnvState", "OutdoorAmbientColor", Kind::Colour},
    {"EnvState", "SkyBoxAngle", Kind::Float},
    {"EnvState", "EyeAdaptationEnable", Kind::Bool},
    {"EnvState", "EyeAdaptationMin", Kind::Float},
    {"EnvState", "EyeAdaptationMax", Kind::Float},
    {"EnvState", "WindForce", Kind::Float},
    {"SectorParameters", "FogDistanceSpeed", Kind::Float},
    {"SectorParameters", "OceanColorSpeed", Kind::Float},
    {"SectorParameters", "OceanReflectAmountSpeed", Kind::Float},
    {"SectorParameters", "OceanReflectAngleSpeed", Kind::Float},
    {"SectorParameters", "OceanScaledUdVSpeed", Kind::Float},
    {"SectorParameters", "ShadowRadiusSpeed", Kind::Float},
    {"SectorParameters", "GlowIntensitySpeed", Kind::Float},
    {"Rain", "Enable", Kind::Bool},
    {"Rain", "Scale", Kind::Float},
    {"Rain", "ScaleMax", Kind::Float},
    {"Rain", "Texture", Kind::Texture},
    {"Rain", "RollAngleMin", Kind::Float},
    {"Rain", "RollAngleMax", Kind::Float},
    {"Rain", "UTiling", Kind::Float},
    {"Rain", "VTiling", Kind::Float},
    {"Rain", "UTilingMax", Kind::Float},
    {"Rain", "VTilingMax", Kind::Float},
    {"Rain", "UScrollingSpeed", Kind::Float},
    {"Rain", "VScrollingSpeed", Kind::Float},
    {"Rain", "UScrollingSpeedMax", Kind::Float},
    {"Rain", "VScrollingSpeedMax", Kind::Float},
    {"Rain", "ParticlesOnGround", Kind::Effect},
    {"Rain", "ParticlesOnWater", Kind::Effect},
    {"Rain", "ParticleSpawnPerSeconds", Kind::Float},
    {"Rain", "ParticleSpawnRadius", Kind::Float},
    {"Rain", "Count", Kind::Int},
    {"Rain", "LayerSpacing", Kind::Float},
    {"Rain", "LayerOffset", Kind::Float},
    {"Rain", "LayerFadeInOutTime", Kind::Float},
    {"Rain", "LayerFallOff", Kind::Float},
    {"Rain", "LayerIntensity", Kind::Float},
    {"Rain", "AngleTransitionDurationMin", Kind::Float},
    {"Rain", "AngleTransitionDurationMax", Kind::Float},
    {"Rain", "TimeBetweenTransitionsMin", Kind::Float},
    {"Rain", "TimeBetweenTransitionsMax", Kind::Float},
    {"Rain", "WaterLevelAdjustement", Kind::Float},
    {"Shadow", "DynamicShadowRadius", Kind::Float},
    {"Shadow", "SunShadowZOffset", Kind::Float},
    {"Shadow", "SunShadowZOffsetSlopeScale", Kind::Float},
    {"Smog", "Amount", Kind::Float},
    {"Smog", "Color", Kind::Colour},
    {"Smog", "Texture", Kind::Texture},
    {"Smog", "SpeedX", Kind::Float},
    {"Smog", "SpeedY", Kind::Float},
    {"Smog", "SpeedZ", Kind::Float},
    {"Smog", "SpeedVariation", Kind::Float},
    {"Smog", "Distance", Kind::Float},
    {"Smog", "FadeIn", Kind::Float},
    {"Smog", "FadeOut", Kind::Float},
    {"Smog", "AngleDegrees", Kind::Float},
    {"Smog", "AngleVariation", Kind::Float},
    {"Smog", "NumberParticles", Kind::Int},
    {"Smog", "SizeParticles", Kind::Float},
    {"Smog", "SizeVariation", Kind::Float},
    {"Ocean", "ShoreSize", Kind::Float},
    {"Ocean", "BeachColor", Kind::Colour},
    {"Ocean", "WaveTexture", Kind::Texture},
    {"Ocean", "WaterTexture", Kind::Texture},
    {"Ocean", "BeachPeriodFactor", Kind::Float},
    {"Ocean", "SurfaceTranspRatio", Kind::Float},
    {"Ocean", "BorderTranspRatio", Kind::Float},
    {"Ocean", "FogColor", Kind::Colour},
    {"Ocean", "FogDistance", Kind::Float},
    {"Ocean", "FogPlaneOffset", Kind::Float},
    {"Ocean", "FogPlaneRange", Kind::Float},
    {"Ocean", "OverBaseAlpha", Kind::Float},
    {"Ocean", "UnderBaseAlpha", Kind::Float},
    {"Ocean", "OverColor", Kind::Colour},
    {"Ocean", "ReflectAngle", Kind::Float},
    {"Ocean", "ReflectAmount", Kind::Float},
    {"Ocean", "Scale_dUdV", Kind::Vector2},
    {"Ocean", "WaveSpeed", Kind::Float},
    {"Ocean", "GlowBoostLevel", Kind::Float},
    {"Ocean", "OceanReflectionLod", Kind::Int},
    {"HeightMap", "GeometryLodRatio", Kind::Float},
    {"HeightMap", "TextureLodRatio", Kind::Float},
    {"Lighting", "OffsetRenderHeight", Kind::Bool},
    {"Lighting", "ShadowIntensity", Kind::Float},
};

// guest scratch: two texts, then the XmlNodeRef the loader takes
constexpr uint32_t kScratchKey = 0x000;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = 0x040;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchRef = 0x140;
constexpr uint32_t kScratchBytes = 0x144;

constexpr float kPi = 3.14159265f;

// one value of a node under Environment
struct Value {
    std::string node;
    std::string key;
    std::string value;
};
// the sun as the PC editor keeps it: rotation in degrees, heights from 0 to 100
struct Sun {
    float rotation = 0;
    float height = 0;
    float render_height = 0;
};
using Edit = std::function<void(PPCContext&)>;

struct Snapshot {
    bool kept = false;
    std::map<std::string, std::vector<XmlAttribute>> nodes;  // by tag
    uint32_t generation = 0;
};

std::mutex g_lock;
Snapshot g_snapshot;
std::vector<Edit> g_edits;
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown = 0;
bool g_stale = false;
// window thread
Snapshot g_view;

// Editor thread

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

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
        if (GuestString(ChildAt(node, i) + kNodeTag) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t EnvironmentNode() { return ChildTagged(KeptMission(), kEnvironment); }

// the engine loads the mission's environment again
void Reload(PPCContext& ctx) {
    W32(g_scratch + kScratchRef, KeptMission());
    CallGuest(ctx, SYM_Environment_Load, {R32(SYM_C3DEngine_pointer), g_scratch + kScratchRef});
}

// values set together, the environment loaded again; one step of undo
void SetValues(PPCContext& ctx, const std::vector<Value>& values, bool undoable) {
    uint32_t environment = EnvironmentNode();
    if (!environment) return;
    std::vector<Value> old;
    for (const Value& v : values) {
        uint32_t node = ChildTagged(environment, v.node);
        if (!node) continue;
        old.push_back({v.node, v.key, Find(Attributes(node), v.key)});
        uint32_t key = WriteText(g_scratch + kScratchKey, v.key, kScratchKeyBytes);
        uint32_t text = WriteText(g_scratch + kScratchValue, v.value, kScratchValueBytes);
        CallGuest(ctx, Method(node, kNodeSetAttr), {node, key, text});
    }
    if (old.empty()) return;
    Reload(ctx);
    g_stale = true;
    NoteMapChanged();
    if (!undoable) return;
    std::string label = "Set " + values.front().node + " " + values.front().key;
    PushUndo(ctx, label, [old](PPCContext& ctx) { SetValues(ctx, old, false); });
}

Snapshot ReadEnvironment() {
    Snapshot s;
    uint32_t environment = EnvironmentNode();
    s.kept = environment != 0;
    for (uint32_t i = 0; environment && i < ChildCount(environment); i++) {
        uint32_t node = ChildAt(environment, i);
        s.nodes[GuestString(node + kNodeTag)] = Attributes(node);
    }
    return s;
}

// Panel, window thread

void Queue(std::vector<Value> values) {
    std::lock_guard lock(g_lock);
    g_edits.push_back([values](PPCContext& ctx) { SetValues(ctx, values, true); });
}

std::string ValueOf(const std::string& node, const std::string& key) {
    auto found = g_view.nodes.find(node);
    return found == g_view.nodes.end() ? std::string() : Find(found->second, key);
}

void Row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 13);
    ImGui::SetNextItemWidth(-1);
}

FieldKind FieldKindOf(Kind kind) {
    switch (kind) {
        case Kind::Bool: return FieldKind::Bool;
        case Kind::Int: return FieldKind::Int;
        case Kind::Float: return FieldKind::Float;
        case Kind::Colour: return FieldKind::Colour255;
        case Kind::Vector2: return FieldKind::Vector2;
        default: return FieldKind::Text;
    }
}

void DrawFieldRow(const Field& f) {
    Row(f.key);
    std::string value = ValueOf(f.node, f.key);
    std::string edited;
    bool done = false;
    if (f.kind == Kind::Texture) done = DrawPicker(f.key, kEnvironmentTextures, value, edited);
    else if (f.kind == Kind::Effect) done = DrawPicker(f.key, kEffects, value, edited);
    else done = DrawField(f.key, FieldKindOf(f.kind), value, edited);
    if (done) Queue({{f.node, f.key, edited}});
}

Sun ReadSun() {
    float v[3] = {};
    float r[3] = {};
    std::sscanf(ValueOf(kLighting, "SunVector").c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
    std::sscanf(ValueOf(kLighting, "SunVectorRender").c_str(), "%f,%f,%f", &r[0], &r[1], &r[2]);
    float rotation = std::atan2(v[0], v[1]) * 180.0f / kPi;
    if (rotation < 0) rotation += 360.0f;
    return {rotation, -v[2] * 100.0f, -r[2] * 100.0f};
}

std::string SunVector(float rotation, float height) {
    float z = -height / 100.0f;
    float h = std::sqrt(std::max(0.0f, 1.0f - z * z));
    float a = rotation * kPi / 180.0f;
    char text[64];
    std::snprintf(text, sizeof(text), "%g,%g,%g", std::sin(a) * h, std::cos(a) * h, z);
    return text;
}

std::string Number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

void QueueSun(const Sun& s) {
    bool linked = s.height == s.render_height;
    Queue({{kLighting, "SunRotation", Number(s.rotation)},
           {kLighting, "SunHeight", Number(s.height)},
           {kLighting, "SunHeightRender", Number(s.render_height)},
           {kLighting, "LinkSunHeightRender", linked ? "1" : "0"},
           {kLighting, "SunVector", SunVector(s.rotation, s.height)},
           {kLighting, "SunVectorRender", SunVector(s.rotation, s.render_height)}});
}

// the time of day: where the sun stands
void DrawSun() {
    if (!ImGui::CollapsingHeader("Sun and Time of Day", ImGuiTreeNodeFlags_DefaultOpen)) return;
    Sun s = ReadSun();
    std::string edited;
    Row("Rotation");
    if (DrawField("rotation", FieldKind::Float, Number(s.rotation), edited)) {
        s.rotation = std::fmod(std::fmod(std::strtof(edited.c_str(), nullptr), 360.0f) + 360.0f, 360.0f);
        QueueSun(s);
    }
    Row("Height");
    if (DrawField("height", FieldKind::Float, Number(s.height), edited)) {
        bool linked = s.height == s.render_height;
        s.height = std::clamp(std::strtof(edited.c_str(), nullptr), 0.0f, 100.0f);
        if (linked) s.render_height = s.height;
        QueueSun(s);
    }
    Row("Render Height");
    if (DrawField("render", FieldKind::Float, Number(s.render_height), edited)) {
        s.render_height = std::clamp(std::strtof(edited.c_str(), nullptr), 0.0f, 100.0f);
        QueueSun(s);
    }
}

void DrawFields() {
    std::string open;
    bool shown = false;
    for (const Field& f : kFields) {
        if (open != f.node) {
            open = f.node;
            shown = g_view.nodes.count(f.node) && ImGui::CollapsingHeader(f.node);
        }
        if (!shown) continue;
        ImGui::PushID(f.node);
        DrawFieldRow(f);
        ImGui::PopID();
    }
}

}  // namespace

void fcip::editor::UpdateEnvironment(PPCContext& ctx) {
    if (!EnsureScratch()) return;
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    if (KeptMission() == g_shown && !g_stale) return;
    Snapshot s = ReadEnvironment();
    g_shown = KeptMission();
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

void fcip::editor::DrawEnvironmentPanel() {
    {
        std::lock_guard lock(g_lock);
        if (g_snapshot.generation != g_view.generation) g_view = g_snapshot;
    }
    ImGui::TextUnformatted("ENVIRONMENT");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("The mission's environment, saved with the level");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!g_view.kept) {
        ImGui::TextUnformatted("Only a campaign or custom level keeps its mission's environment.");
        return;
    }
    ImGui::BeginChild("##environment");
    DrawSun();
    DrawFields();
    ImGui::EndChild();
}
