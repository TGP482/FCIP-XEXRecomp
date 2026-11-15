// ige.xex, the solo level editor: the scripted moments. Each trigger shows what the game tests. The Dim triggers
// (Proximity, EndMap, Utility, Environment) get the box their script's SetLocalBBox gives, as the engine's GetBBox
// (+0xA0) has it: the axis aligned box around the turned one, which their trigger area uses. An AreaTrigger gets the
// mission areas (Shape, AreaBox, AreaSphere) naming it in their Entities, which tell it of the player entering. Delay,
// Multiple and Blinking triggers have no volume, they fire from their inputs. A line goes from each trigger to what
// its events reach: another entity, or for a mission call (EventTarget 0) the anchor (_GenericJob_EnableAnchor,
// _GenericJob_DisableAnchor) or entity (_Player_BindEyes) it names; the other calls and a PlaySequence are listed
// under its name when the camera is near.
#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "leveleditor/event_links.h"
#include "leveleditor/markers.h"

using namespace fcip::editor;

namespace {

constexpr const char* kTriggerClasses[] = {"ProximityTrigger", "ProximityTrigger_MP", "AreaTrigger",
                                           "MultipleTrigger",  "DelayTrigger",        "EndMapTrigger",
                                           "UtilityTrigger",   "EnvironmentTrigger",  "BlinkingTrigger"};
// the Properties shown under a trigger's name, when it has them
constexpr const char* kShownProperties[] = {"PlaySequence", "Delay", "iNumInputs", "NextMap", "bOnlyPlayer",
                                            "bOnlyVehicle", "bTriggerOnce"};

constexpr ImU32 kTriggerColour = IM_COL32(250, 160, 50, 255);
constexpr ImU32 kDisabledColour = IM_COL32(250, 160, 50, 110);
constexpr ImU32 kLinkColour = IM_COL32(250, 220, 120, 200);

struct Lookup {
    std::unordered_map<uint32_t, const ViewEntity*> by_id;
    std::unordered_map<std::string, const ViewEntity*> by_name;
    std::unordered_map<std::string, const AiObject*> objects;     // by name
    std::unordered_multimap<std::string, const AiObject*> areas;  // by the entity they tell
};

// the name of what a mission call reaches, empty when it names none
std::string CallTarget(const Link& l) {
    if (l.event == "_GenericJob_EnableAnchor" || l.event == "_GenericJob_DisableAnchor") return l.param;
    if (l.event == "_Player_BindEyes") return l.param.substr(0, l.param.find(','));  // "entity,bone"
    return {};
}

const float* PlaceOf(const Lookup& look, const std::string& name) {
    auto entity = look.by_name.find(name);
    if (entity != look.by_name.end()) return entity->second->pos;
    auto object = look.objects.find(name);
    if (object != look.objects.end() && object->second->placed) return object->second->pos;
    return nullptr;
}

void DrawLink(const View& v, const float from[3], const float to[3], const std::string& text) {
    Arrow(v, from, to, kLinkColour);
    float middle[3];
    for (int i = 0; i < 3; i++) middle[i] = (from[i] + to[i]) * 0.5f;
    if (NearCamera(v, middle)) Label(v, middle, text.c_str(), kLinkColour);
}

// links drawn as lines; the mission calls with nothing to point at come back as text
std::string DrawLinks(const View& v, const ViewEntity& e, const Lookup& look) {
    std::string calls;
    for (const Link& l : LinksOf(e.id)) {
        std::string text = l.source + " > " + l.event;
        if (l.target) {
            auto target = look.by_id.find(l.target);
            if (target != look.by_id.end()) DrawLink(v, e.pos, target->second->pos, text);
            continue;
        }
        const float* at = PlaceOf(look, CallTarget(l));
        if (at) DrawLink(v, e.pos, at, text + " " + l.param);
        else calls += "\n" + text + (l.param.empty() ? "" : " " + l.param);
    }
    return calls;
}

void DrawAreas(const View& v, const ViewEntity& e, const Lookup& look, ImU32 colour) {
    auto [first, last] = look.areas.equal_range(e.name);
    for (auto it = first; it != last; ++it) {
        const AiObject& o = *it->second;
        DrawArea(v, o, colour);
        const float* at = o.placed ? o.pos : (o.points.empty() ? nullptr : o.points.front().data());
        if (at) Line(v, at, e.pos, colour);
    }
}

std::string Details(const ViewEntity& e) {
    std::string text = e.name + " (" + e.cls + ")";
    for (const char* key : kShownProperties) {
        std::string value = PropertyOf(e, key);
        if (!value.empty()) text += "\n" + std::string(key) + " " + value;
    }
    return text;
}

void DrawTrigger(const View& v, const ViewEntity& e, const Lookup& look) {
    ImU32 colour = PropertyOf(e, "bEnabled") == "0" ? kDisabledColour : kTriggerColour;
    if (e.cls == "AreaTrigger") DrawAreas(v, e, look, colour);
    else if (e.has_box) AxisBox(v, e.box_min, e.box_max, colour);
    Square(v, e.pos, colour);
    std::string calls = DrawLinks(v, e, look);
    if (NearCamera(v, e.pos)) Label(v, e.pos, (Details(e) + calls).c_str(), colour);
}

}  // namespace

bool fcip::editor::IsTrigger(const std::string& cls) {
    return std::find(std::begin(kTriggerClasses), std::end(kTriggerClasses), cls) != std::end(kTriggerClasses);
}

void fcip::editor::DrawTriggers(const View& v) {
    std::shared_ptr<const std::vector<ViewEntity>> entities = ViewEntities();
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    Lookup look;
    for (const ViewEntity& e : *entities) {
        look.by_id[e.id] = &e;
        look.by_name[e.name] = &e;
    }
    for (const AiObject& o : ai->objects) {
        look.objects[o.name] = &o;
        for (const std::string& name : o.entities) look.areas.emplace(name, &o);
    }
    for (const ViewEntity& e : *entities) {
        if (IsTrigger(e.cls)) DrawTrigger(v, e, look);
    }
}
