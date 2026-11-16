// ige.xex, the solo level editor: the Level tab edits the LevelData.xml a campaign level keeps for the save
// (mission.cpp): its water level and its terrain surface types. The engine reads both as a level loads: LevelInfo
// WaterLevel into the terrain (82469E50), and each SurfaceType through 823611F8, which hands its values to
// CTerrain::SetDetailTextures (82469CE0) by its place among the SurfaceTypes children. An edit sets the kept node and
// gives the engine the same values, so the view shows it at once. The Vegetation list beside them is not read by the
// game (its vegetation comes from vegbank.dat), so it is left as the level has it.
#include "leveleditor/level_data.h"

#include <bit>
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

constexpr const char* kLevelInfo = "LevelInfo";
constexpr const char* kWaterLevel = "WaterLevel";
constexpr const char* kSurfaceTypes = "SurfaceTypes";
constexpr const char* kSurfaceType = "SurfaceType";
constexpr const char* kName = "Name";
constexpr const char* kDetailTexture = "DetailTexture";
constexpr const char* kScaleX = "DetailScaleX";
constexpr const char* kScaleY = "DetailScaleY";
constexpr const char* kProjAxis = "ProjAxis";
constexpr const char* kEnableGrass = "EnableGrass";
constexpr const char* kMaterialId = "MaterialID";
constexpr const char* kGrassDensity = "GrassTileDensity";  // with the grass type, 0 to 7
constexpr int kGrassTypes = 8;
constexpr int kMaxSurfaces = 8;  // SetDetailTextures refuses a layer past 7
constexpr const char* kAxes[] = {"X", "Y", "Z"};

// the 3D engine's terrain, its water level as LevelInfo loads it
constexpr uint32_t kEngineTerrain = 0x364;
constexpr uint32_t kTerrainWaterLevel = 0xA4;

// guest scratch: two texts for node calls, then SetDetailTextures' name, texture and grass densities
constexpr uint32_t kScratchKey = 0x000;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = 0x040;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchName = 0x140;
constexpr uint32_t kScratchNameBytes = 0x40;
constexpr uint32_t kScratchTexture = 0x180;
constexpr uint32_t kScratchTextureBytes = 0x100;
constexpr uint32_t kScratchDensities = 0x280;
constexpr uint32_t kScratchRef = 0x2A0;
constexpr uint32_t kScratchBytes = 0x2A4;

using Edit = std::function<void(PPCContext&)>;

struct Snapshot {
    bool kept = false;  // a LevelData with SurfaceTypes is kept
    std::string water;
    std::vector<std::vector<XmlAttribute>> surfaces;  // in SurfaceTypes order, the engine's layer numbers
    uint32_t generation = 0;
};

std::mutex g_lock;
Snapshot g_snapshot;
std::vector<Edit> g_edits;
// editor thread
uint32_t g_scratch = 0;
uint32_t g_shown = 0;  // the kept LevelData the snapshot holds
bool g_stale = false;
// window thread
Snapshot g_view;

// Editor thread

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) { return WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes); }
uint32_t ValueText(const std::string& text) { return WriteText(g_scratch + kScratchValue, text, kScratchValueBytes); }

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

bool Has(const std::vector<XmlAttribute>& attributes, const std::string& key) {
    for (const XmlAttribute& a : attributes) {
        if (a.key == key) return true;
    }
    return false;
}

uint32_t ChildTagged(uint32_t node, const char* tag) {
    for (uint32_t i = 0; node && i < ChildCount(node); i++) {
        if (GuestString(ChildAt(node, i) + kNodeTag) == tag) return ChildAt(node, i);
    }
    return 0;
}

uint32_t InfoNode() { return ChildTagged(KeptLevelData(), kLevelInfo); }
uint32_t SurfacesNode() { return ChildTagged(KeptLevelData(), kSurfaceTypes); }

uint32_t SurfaceAt(int index) {
    uint32_t surfaces = SurfacesNode();
    if (!surfaces || index < 0 || uint32_t(index) >= ChildCount(surfaces)) return 0;
    return ChildAt(surfaces, uint32_t(index));
}

uint32_t Terrain() {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    return engine ? R32(engine + kEngineTerrain) : 0;
}

void SetAttr(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value) {
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

// the engine takes a surface type as 823611F8 loads it, which skips one without a texture, a name or both scales
void ApplySurface(PPCContext& ctx, int index) {
    uint32_t node = SurfaceAt(index);
    uint32_t terrain = Terrain();
    if (!node || !terrain) return;
    std::vector<XmlAttribute> attributes = Attributes(node);
    std::string texture = Find(attributes, kDetailTexture);
    std::string name = Find(attributes, kName);
    if (texture.empty() || name.empty() || !Has(attributes, kScaleX) || !Has(attributes, kScaleY)) return;
    for (int i = 0; i < kGrassTypes; i++) {
        std::string density = Find(attributes, kGrassDensity + std::to_string(i));
        float value = std::strtof(density.c_str(), nullptr);
        W32(g_scratch + kScratchDensities + uint32_t(i) * 4, std::bit_cast<uint32_t>(value));
    }
    std::string axis = Find(attributes, kProjAxis);
    uint32_t material = uint32_t(std::atoi(Find(attributes, kMaterialId).c_str()));
    uint32_t grass = std::atoi(Find(attributes, kEnableGrass).c_str()) != 0;
    double f1 = ctx.f[1];
    double f2 = ctx.f[2];
    ctx.f[1] = std::strtof(Find(attributes, kScaleX).c_str(), nullptr);
    ctx.f[2] = std::strtof(Find(attributes, kScaleY).c_str(), nullptr);
    uint32_t name_text = WriteText(g_scratch + kScratchName, name, kScratchNameBytes);
    uint32_t texture_text = WriteText(g_scratch + kScratchTexture, texture, kScratchTextureBytes);
    // (terrain, layer, texture, unused, material, axis letter, grass, densities, name, material)
    CallGuest(ctx, SYM_Terrain_SetDetailTextures,
              {terrain, uint32_t(index), texture_text, 0, material, uint32_t(uint8_t(axis[0])), grass,
               g_scratch + kScratchDensities, name_text, material});
    ctx.f[1] = f1;
    ctx.f[2] = f2;
}

void ApplyWater() {
    uint32_t terrain = Terrain();
    if (!terrain) return;
    float level = std::strtof(Find(Attributes(InfoNode()), kWaterLevel).c_str(), nullptr);
    W32(terrain + kTerrainWaterLevel, std::bit_cast<uint32_t>(level));
}

// a LevelInfo (surface -1) or SurfaceType value set and given to the engine; one step of undo
void SetValue(PPCContext& ctx, int surface, const std::string& key, const std::string& value, bool undoable) {
    uint32_t node = surface < 0 ? InfoNode() : SurfaceAt(surface);
    if (!node) return;
    std::string old = Find(Attributes(node), key);
    SetAttr(ctx, node, key, value);
    if (surface < 0) ApplyWater();
    else ApplySurface(ctx, surface);
    g_stale = true;
    NoteMapChanged();
    if (!undoable || old == value) return;
    PushUndo(ctx, "Set " + key, [=](PPCContext& ctx) { SetValue(ctx, surface, key, old, false); });
}

void RemoveChild(PPCContext& ctx, uint32_t parent, uint32_t child) {
    CallGuest(ctx, Method(child, kNodeAddRef), {child});
    W32(g_scratch + kScratchRef, child);
    CallGuest(ctx, Method(parent, kNodeRemoveChild), {parent, g_scratch + kScratchRef});
}

// a copy of the last surface type under a new name, given to the engine as the next layer; undo takes the node away,
// the engine's layer stays loaded until the level opens again
void AddSurface(PPCContext& ctx) {
    uint32_t surfaces = SurfacesNode();
    uint32_t count = surfaces ? ChildCount(surfaces) : 0;
    if (!surfaces || count >= uint32_t(kMaxSurfaces)) return;
    std::vector<XmlAttribute> attributes = Attributes(count ? ChildAt(surfaces, count - 1) : 0);
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(surfaces, kNodeNewChild), {g_scratch + kScratchRef, surfaces, KeyText(kSurfaceType)});
    uint32_t added = R32(g_scratch + kScratchRef);
    if (!added) return;
    CallGuest(ctx, Method(added, kNodeRelease), {added});
    for (const XmlAttribute& a : attributes) SetAttr(ctx, added, a.key, a.value);
    SetAttr(ctx, added, kName, "Surface" + std::to_string(count));
    ApplySurface(ctx, int(count));
    g_stale = true;
    NoteMapChanged();
    PushUndo(ctx, "Add a surface type", [surfaces, added](PPCContext& ctx) {
        if (SurfacesNode() != surfaces || ChildCount(surfaces) == 0) return;
        if (ChildAt(surfaces, ChildCount(surfaces) - 1) != added) return;
        RemoveChild(ctx, surfaces, added);
        g_stale = true;
    });
}

Snapshot ReadLevelData() {
    Snapshot s;
    uint32_t surfaces = SurfacesNode();
    s.kept = surfaces != 0;
    s.water = Find(Attributes(InfoNode()), kWaterLevel);
    for (uint32_t i = 0; surfaces && i < ChildCount(surfaces); i++) {
        s.surfaces.push_back(Attributes(ChildAt(surfaces, i)));
    }
    return s;
}

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

// Panel, window thread

void QueueValue(int surface, const char* key, std::string value) {
    Queue([=](PPCContext& ctx) { SetValue(ctx, surface, key, value, true); });
}

void Row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 10);
    ImGui::SetNextItemWidth(-1);
}

void ValueRow(int surface, const std::vector<XmlAttribute>& attributes, const char* key, FieldKind kind) {
    Row(key);
    std::string edited;
    if (DrawField(key, kind, Find(attributes, key), edited)) QueueValue(surface, key, edited);
}

void AxisRow(int surface, const std::vector<XmlAttribute>& attributes) {
    Row(kProjAxis);
    std::string axis = Find(attributes, kProjAxis);
    if (!ImGui::BeginCombo("##axis", axis.c_str())) return;
    for (const char* a : kAxes) {
        if (ImGui::Selectable(a, axis == a)) QueueValue(surface, kProjAxis, a);
    }
    ImGui::EndCombo();
}

void DrawSurface(int index, const std::vector<XmlAttribute>& attributes) {
    std::string title = Find(attributes, kName) + "##surface" + std::to_string(index);
    if (!ImGui::CollapsingHeader(title.c_str())) return;
    ImGui::PushID(index);
    ValueRow(index, attributes, kName, FieldKind::Text);
    Row(kDetailTexture);
    std::string edited;
    if (DrawPicker(kDetailTexture, kDetailTextures, Find(attributes, kDetailTexture), edited)) {
        QueueValue(index, kDetailTexture, edited);
    }
    ValueRow(index, attributes, kScaleX, FieldKind::Float);
    ValueRow(index, attributes, kScaleY, FieldKind::Float);
    AxisRow(index, attributes);
    ValueRow(index, attributes, kMaterialId, FieldKind::Int);
    ValueRow(index, attributes, kEnableGrass, FieldKind::Bool);
    for (int i = 0; i < kGrassTypes; i++) {
        std::string key = kGrassDensity + std::to_string(i);
        ValueRow(index, attributes, key.c_str(), FieldKind::Float);
    }
    ImGui::PopID();
}

void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kHeading);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

}  // namespace

void fcip::editor::UpdateLevelData(PPCContext& ctx) {
    if (!EnsureScratch()) return;
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    if (KeptLevelData() == g_shown && !g_stale) return;
    Snapshot s = ReadLevelData();
    g_shown = KeptLevelData();
    g_stale = false;
    std::lock_guard lock(g_lock);
    s.generation = g_snapshot.generation + 1;
    g_snapshot = std::move(s);
}

void fcip::editor::DrawLevelPanel() {
    {
        std::lock_guard lock(g_lock);
        if (g_snapshot.generation != g_view.generation) g_view = g_snapshot;
    }
    ImGui::TextUnformatted("LEVEL");
    ImGui::PushStyleColor(ImGuiCol_Text, fcip::kDim);
    ImGui::TextUnformatted("LevelData.xml, saved with the level");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (!g_view.kept) {
        ImGui::TextUnformatted("Only a campaign or custom level has its own LevelData.");
        return;
    }
    ImGui::BeginChild("##level");
    Heading("Water");
    ValueRow(-1, {{kWaterLevel, g_view.water}}, kWaterLevel, FieldKind::Float);
    ImGui::Spacing();
    Heading("Surface Types");
    for (size_t i = 0; i < g_view.surfaces.size(); i++) DrawSurface(int(i), g_view.surfaces[i]);
    ImGui::BeginDisabled(g_view.surfaces.size() >= size_t(kMaxSurfaces));
    if (ImGui::Button("Add Surface Type")) Queue(AddSurface);
    ImGui::EndDisabled();
    ImGui::EndChild();
}
