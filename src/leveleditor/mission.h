// a campaign level's mission XML and LevelData.xml, kept after it loads for its AI objects, entity layers and the save
// (mission.cpp)
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct PPCContext;

namespace fcip::editor {

struct XmlAttribute {
    std::string key;
    std::string value;
};
// a node of an AI object: the object itself (no path), then its children depth first
struct XmlElement {
    int depth = 0;
    std::vector<int> path;  // child indices from the object
    std::string tag;
    std::vector<XmlAttribute> attributes;
};
// an <Object> of the mission: a tag point, an anchor and its actions, a shape, an area
struct AiObject {
    uint32_t node = 0;  // guest, the object's identity
    std::string type;
    std::string name;
    bool placed = false;  // has a Pos
    float pos[3] = {};
    std::vector<std::array<float, 3>> points;  // a shape's or path's
    std::vector<std::string> entities;         // an area's: the entities it tells when something enters
    std::vector<XmlElement> elements;
};

// the object's own attribute, empty when it has none
inline std::string AttributeOf(const AiObject& o, const char* key) {
    for (const XmlAttribute& a : o.elements.front().attributes) {
        if (a.key == key) return a.value;
    }
    return {};
}
struct AiObjects {
    uint32_t generation = 0;
    bool kept = false;  // a mission is kept, so objects can be added
    std::vector<AiObject> objects;
    std::unordered_map<uint32_t, std::string> layers;  // each mission entity's Layer, by EntityId
};

// editor thread
void KeepMission(PPCContext& ctx, uint32_t node);    // a campaign level's mission, as its objects load
void KeepLevelData(PPCContext& ctx, uint32_t node);  // its LevelData.xml, as it loads
void ForgetMission(PPCContext& ctx);                  // a level starts
uint32_t KeptLevelData();
// the kept mission for a custom level: its Objects rebuilt from the editor's entities (82104C38 on the mission object)
// and the kept AI objects; referenced once, 0 without a kept mission
uint32_t MissionToSave(PPCContext& ctx, uint32_t mission_object);
void UpdateMission(PPCContext& ctx);                  // each frame: queued edits, then a fresh snapshot after any

// any thread
std::shared_ptr<const AiObjects> ReadAiObjects();
std::shared_ptr<const AiObjects> ReadLoadedAiObjects();  // as the level loaded them, before any edit
std::vector<std::string> AiObjectNames(const std::string& type);  // sorted
std::string LayerOf(uint32_t entity_id);  // empty for an entity the mission does not place

// window thread: edits the editor thread applies at its next update, each one step of undo
void SetAiAttribute(uint32_t object, std::vector<int> path, std::string key, std::string value);
void AddAiChild(uint32_t object, std::vector<int> path, std::string tag);
void RemoveAiChild(uint32_t object, std::vector<int> path);
void MoveAiChild(uint32_t object, std::vector<int> path, int delta);  // among its siblings
void AddAiObject(std::string type, std::array<float, 3> pos);  // any of kLevelObjects (level_objects.h)
void RemoveAiObject(uint32_t object);
// moved, its Angles turned by degrees, its points with it (turned about its Pos by the z turn)
void PlaceAiObject(uint32_t object, std::array<float, 3> move, std::array<float, 3> turn);
void SetAiPoints(uint32_t object, std::vector<std::array<float, 3>> points);  // a shape's or area's, in order
void SetAiEntities(uint32_t object, std::vector<std::string> names);          // the entities an area tells

}  // namespace fcip::editor
