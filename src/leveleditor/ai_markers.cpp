// ige.xex, the solo level editor: what the AI runs on. The mission's Objects load into the AI system as the level
// starts (default.xex 82136CB0): tag points and anchors facing their Angles, AIPath points; an anchor's NextAnchor,
// Try OnFailAnchor and SwitchToAnchor lead to other anchors, and its Move goes to its Target, or to the anchor itself
// when it names none. Enemies and AI vehicles go to their FirstAnchor (FirstAnchor1, FirstAnchor2), OnPlayerSeenAnchor
// and OnCoolDownAnchor, vehicles drive their pathname. A CreatureGeneratorSpawnSpot spawns for the CreatureGenerator
// its NameOfSpawnEntity names, within fSpawnRadius. ForbiddenArea, EnclosingArea, FenceArea and AINavigationModifier
// are what the AI navigation graph was built from; the areas naming no entity are the mission's indoor and sound
// areas. An anchor's other actions are drawn while it is near the camera or picked in the AI tab.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/panel_colors.h"
#include "leveleditor/ai_panel.h"
#include "leveleditor/markers.h"

using namespace fcip::editor;

namespace {

// the classes the missions give per instance AI properties (Properties2)
constexpr const char* kAiClasses[] = {
    "Merc", "Freak", "Valerie", "Dragon", "Worker", "Sapper", "Chucker",
    "Buggy", "PickupTruck", "Humvee", "Gunship", "Boat", "JetSki", "Zodiac", "Sampan", "Paraglider", "Hovercraft",
    "Unimog", "CreatureGeneratorSpawnSpot",
};
constexpr const char* kAnchorFields[] = {"FirstAnchor",         "FirstAnchor1",        "FirstAnchor2",
                                         "OnPlayerSeenAnchor",  "OnPlayerSeenAnchor1", "OnPlayerSeenAnchor2",
                                         "OnCoolDownAnchor"};
// an anchor's actions that name another object as their Target
constexpr const char* kTargetedActions[] = {"OrientTo", "LookAt", "ShootAt", "Relocate", "JumpTo", "Say",
                                            "SendSignal", "Action"};
struct NavArea {
    const char* type;
    ImU32 colour;
};
constexpr NavArea kNavAreas[] = {
    {"ForbiddenArea", IM_COL32(235, 70, 70, 255)},
    {"EnclosingArea", IM_COL32(90, 150, 255, 255)},
    {"FenceArea", IM_COL32(220, 150, 80, 255)},
    {"AINavigationModifier", IM_COL32(190, 110, 240, 255)},
};

constexpr ImU32 kAnchorColour = IM_COL32(245, 215, 70, 255);
constexpr ImU32 kTagPointColour = IM_COL32(245, 235, 160, 255);
constexpr ImU32 kPathColour = IM_COL32(235, 235, 235, 220);
constexpr ImU32 kActionColour = IM_COL32(245, 215, 70, 140);
constexpr ImU32 kEnemyColour = IM_COL32(240, 85, 70, 255);
constexpr ImU32 kGeneratorColour = IM_COL32(240, 85, 70, 160);
constexpr ImU32 kAreaColour = IM_COL32(160, 160, 160, 150);
constexpr float kFacingMetres = 2.0f;

struct Lookup {
    std::unordered_map<std::string, const AiObject*> objects;  // by name
    std::unordered_map<std::string, const ViewEntity*> entities;
    uint32_t picked = 0;
};

template <size_t N>
bool IsOneOf(const std::string& text, const char* const (&list)[N]) {
    return std::find(std::begin(list), std::end(list), text) != std::end(list);
}

const float* PlaceOf(const Lookup& look, const std::string& name) {
    if (name.empty()) return nullptr;
    auto object = look.objects.find(name);
    if (object != look.objects.end()) {
        const AiObject& o = *object->second;
        if (o.placed) return o.pos;
        if (!o.points.empty()) return o.points.front().data();  // a path starts at its first point
    }
    auto entity = look.entities.find(name);
    return entity == look.entities.end() ? nullptr : entity->second->pos;
}

std::string ElementAttr(const XmlElement& e, const char* key) {
    for (const XmlAttribute& a : e.attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}

void Facing(const View& v, const AiObject& o, ImU32 colour) {
    float angles[3] = {};
    std::string text = AttributeOf(o, "Angles");
    if (std::sscanf(text.c_str(), "%f,%f,%f", &angles[0], &angles[1], &angles[2]) != 3) return;
    float axes[3][3];
    AxesFromAngles(angles, axes);
    float ahead[3];
    for (int i = 0; i < 3; i++) ahead[i] = o.pos[i] + axes[1][i] * kFacingMetres;
    Arrow(v, o.pos, ahead, colour);
}

void LinkTo(const View& v, const float from[3], const Lookup& look, const std::string& name, ImU32 colour) {
    if (const float* to = PlaceOf(look, name)) Arrow(v, from, to, colour);
}

// the anchor's children are its actions, in the order it runs them
void DrawActions(const View& v, const AiObject& o, const Lookup& look, bool detailed) {
    for (size_t i = 1; i < o.elements.size(); i++) {
        const XmlElement& e = o.elements[i];
        std::string target = ElementAttr(e, "Target");
        if (e.tag == "Move") LinkTo(v, o.pos, look, target, kAnchorColour);
        else if (e.tag == "Try") LinkTo(v, o.pos, look, ElementAttr(e, "OnFailAnchor"), kActionColour);
        else if (e.tag == "SwitchToAnchor") LinkTo(v, o.pos, look, ElementAttr(e, "Name"), kAnchorColour);
        else if (detailed && IsOneOf(e.tag, kTargetedActions)) LinkTo(v, o.pos, look, target, kActionColour);
    }
}

std::string ActionList(const AiObject& o) {
    std::string text;
    for (size_t i = 1; i < o.elements.size(); i++) {
        const XmlElement& e = o.elements[i];
        std::string target = ElementAttr(e, "Target");
        text += "\n" + std::string(size_t(e.depth - 1) * 2, ' ') + e.tag + (target.empty() ? "" : " " + target);
    }
    return text;
}

void DrawAnchor(const View& v, const AiObject& o, const Lookup& look) {
    bool picked = o.node == look.picked;
    bool near = picked || NearCamera(v, o.pos);
    ImU32 colour = picked ? ImGui::GetColorU32(fcip::kText) : kAnchorColour;
    Circle(v, o.pos, 0.5f, colour);
    Facing(v, o, colour);
    LinkTo(v, o.pos, look, AttributeOf(o, "NextAnchor"), colour);
    DrawActions(v, o, look, near);
    if (!near) return;
    std::string text = o.name + " (" + AttributeOf(o, "Action") + ")";
    std::string group = AttributeOf(o, "GroupID");
    if (!group.empty() && group != "-1") text += "\ngroup " + group;
    Label(v, o.pos, (text + ActionList(o)).c_str(), colour);
}

void DrawTagPoint(const View& v, const AiObject& o, const Lookup& look) {
    bool picked = o.node == look.picked;
    ImU32 colour = picked ? ImGui::GetColorU32(fcip::kText) : kTagPointColour;
    Circle(v, o.pos, 0.25f, colour);
    Facing(v, o, colour);
    if (picked || NearCamera(v, o.pos)) Label(v, o.pos, o.name.c_str(), colour);
}

void DrawPath(const View& v, const AiObject& o, const Lookup& look) {
    ImU32 colour = o.node == look.picked ? ImGui::GetColorU32(fcip::kText) : kPathColour;
    for (size_t i = 1; i < o.points.size(); i++) Arrow(v, o.points[i - 1].data(), o.points[i].data(), colour);
    if (AttributeOf(o, "Closed") == "1" && o.points.size() > 2) {
        Arrow(v, o.points.back().data(), o.points.front().data(), colour);
    }
    if (!o.points.empty() && NearCamera(v, o.points.front().data())) {
        Label(v, o.points.front().data(), (o.name + " (AI path)").c_str(), colour);
    }
}

void DrawNavArea(const View& v, const AiObject& o, ImU32 colour) {
    float height = std::strtof(AttributeOf(o, "Height").c_str(), nullptr);
    Prism(v, o.points, height, AttributeOf(o, "Closed") != "0", colour);
    if (!o.points.empty() && NearCamera(v, o.points.front().data())) {
        Label(v, o.points.front().data(), (o.name + " (" + o.type + ")").c_str(), colour);
    }
}

void DrawOtherArea(const View& v, const AiObject& o) {
    DrawArea(v, o, kAreaColour);
    const float* at = o.placed ? o.pos : (o.points.empty() ? nullptr : o.points.front().data());
    if (!at || !NearCamera(v, at)) return;
    std::string text = o.name + " (" + o.type + ")";
    if (AttributeOf(o, "Indoor") == "1") text += "\nIndoor";
    std::string occlusion = AttributeOf(o, "SoundOcclusionFactor");
    if (!occlusion.empty() && occlusion != "0") text += "\nSoundOcclusionFactor " + occlusion;
    Label(v, at, text.c_str(), kAreaColour);
}

void DrawObject(const View& v, const AiObject& o, const Lookup& look) {
    if (o.type == "AIAnchor" && o.placed) return DrawAnchor(v, o, look);
    if (o.type == "TagPoint" && o.placed) return DrawTagPoint(v, o, look);
    if (o.type == "AIPath") return DrawPath(v, o, look);
    for (const NavArea& n : kNavAreas) {
        if (o.type == n.type) return DrawNavArea(v, o, n.colour);
    }
    bool area = o.type == "Shape" || o.type == "AreaBox" || o.type == "AreaSphere";
    if (area && o.entities.empty()) DrawOtherArea(v, o);  // the ones naming entities are their entities'
}

void DrawEntity(const View& v, const ViewEntity& e, const Lookup& look) {
    bool spot = e.cls == "CreatureGeneratorSpawnSpot";
    ImU32 colour = spot ? kGeneratorColour : kEnemyColour;
    Circle(v, e.pos, 0.6f, colour);
    for (const char* field : kAnchorFields) LinkTo(v, e.pos, look, PropertyOf(e, field), colour);
    LinkTo(v, e.pos, look, PropertyOf(e, "pathname"), colour);
    if (spot) {
        LinkTo(v, e.pos, look, PropertyOf(e, "NameOfSpawnEntity"), colour);
        Circle(v, e.pos, std::strtof(PropertyOf(e, "fSpawnRadius").c_str(), nullptr), colour);
    }
    if (!NearCamera(v, e.pos)) return;
    std::string text = e.name + " (" + e.cls + ")";
    std::string group = PropertyOf(e, "groupid");
    if (!group.empty()) text += "\ngroup " + group;
    std::string behaviour = PropertyOf(e, "aibehavior_behaviour");
    if (!behaviour.empty()) text += "\n" + behaviour;
    Label(v, e.pos, text.c_str(), colour);
}

void DrawGenerator(const View& v, const ViewEntity& e) {
    Square(v, e.pos, kGeneratorColour);
    if (NearCamera(v, e.pos)) Label(v, e.pos, (e.name + " (CreatureGenerator)").c_str(), kGeneratorColour);
}

}  // namespace

bool fcip::editor::IsAiEntity(const std::string& cls) { return IsOneOf(cls, kAiClasses); }

void fcip::editor::DrawAi(const View& v) {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    std::shared_ptr<const std::vector<ViewEntity>> entities = ViewEntities();
    Lookup look;
    look.picked = SelectedAiObject();
    for (const AiObject& o : ai->objects) look.objects[o.name] = &o;
    for (const ViewEntity& e : *entities) look.entities[e.name] = &e;
    for (const AiObject& o : ai->objects) DrawObject(v, o, look);
    for (const ViewEntity& e : *entities) {
        if (IsAiEntity(e.cls)) DrawEntity(v, e, look);
        else if (e.cls == "CreatureGenerator") DrawGenerator(v, e);
    }
}
