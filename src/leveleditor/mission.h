// a campaign level's mission XML, kept after it loads for its AI objects (mission.cpp)
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
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
    std::vector<XmlElement> elements;
};
struct AiObjects {
    uint32_t generation = 0;
    std::vector<AiObject> objects;
};

// editor thread
void KeepMission(PPCContext& ctx, uint32_t node);  // a campaign level's mission, as its objects load
void ForgetMission(PPCContext& ctx);                // a level starts
void UpdateMission(PPCContext& ctx);                // each frame: queued edits, then a fresh snapshot after any

// any thread
std::shared_ptr<const AiObjects> ReadAiObjects();
std::vector<std::string> AiObjectNames(const std::string& type);  // sorted

// window thread: edits the editor thread applies at its next update
void SetAiAttribute(uint32_t object, std::vector<int> path, std::string key, std::string value);
void AddAiChild(uint32_t object, std::vector<int> path, std::string tag);
void RemoveAiChild(uint32_t object, std::vector<int> path);
void MoveAiChild(uint32_t object, std::vector<int> path, int delta);  // among its siblings
void AddAiObject(std::string type, std::array<float, 3> pos);       // a TagPoint or an AIAnchor
void RemoveAiObject(uint32_t object);

}  // namespace fcip::editor
