// guest objects as the level editor reads them: methods by vtable offset, std::string, XML nodes and the entities
#pragma once
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace fcip::editor {

// CXmlNode: tag, children (XmlNodeRef), attributes (key and value strings)
constexpr uint32_t kNodeBytes = 0x68;
constexpr uint32_t kNodeTag = 0x0C;
constexpr uint32_t kNodeChildFirst = 0x4C;
constexpr uint32_t kNodeChildLast = 0x50;
constexpr uint32_t kNodeAttrFirst = 0x5C;
constexpr uint32_t kNodeAttrLast = 0x60;
constexpr uint32_t kAttrBytes = 0x38;
constexpr uint32_t kAttrKey = 0x00;
constexpr uint32_t kAttrValue = 0x1C;
// IXmlNode methods
constexpr uint32_t kNodeAddRef = 0x08;
constexpr uint32_t kNodeRelease = 0x0C;
constexpr uint32_t kNodeGetAttr = 0x4C;
constexpr uint32_t kNodeAddChild = 0x58;  // takes an XmlNodeRef by value, which it releases
constexpr uint32_t kNodeNewChild = 0x5C;     // (XmlNodeRef out, tag)
constexpr uint32_t kNodeRemoveChild = 0x60;  // takes an XmlNodeRef by value, which it releases
constexpr uint32_t kNodeChildCount = 0x68;
constexpr uint32_t kNodeGetChild = 0x6C;
constexpr uint32_t kNodeFindChild = 0x70;
constexpr uint32_t kNodeClone = 0x80;   // (XmlNodeRef out, this), a deep copy
constexpr uint32_t kNodeGetXml = 0x8C;  // (this, std::string out, 0)
constexpr uint32_t kNodeSetAttr = 0xB0;  // (key, text)

// entity: methods, then fields
constexpr uint32_t kEntityClassName = 0x04;
constexpr uint32_t kEntityPosition = 0x10;  // (world) returns a Vec3
constexpr uint32_t kEntityBBox = 0xA0;      // (min out, max out) the world box its trigger area and physics use
constexpr uint32_t kEntityId = 0xB8;
constexpr uint32_t kEntityName = 0xBC;  // std::string
constexpr uint32_t kEntityScript = 0x134;
constexpr uint32_t kEntityAngles = 0x374;  // degrees, as SetAngles (82127578) keeps them

inline uint32_t Method(uint32_t object, uint32_t slot) { return kernel::R32(kernel::R32(object) + slot); }

inline uint32_t ChildCount(uint32_t node) {
    uint32_t first = kernel::R32(node + kNodeChildFirst);
    return first ? (kernel::R32(node + kNodeChildLast) - first) / 4 : 0;
}

inline uint32_t ChildAt(uint32_t node, uint32_t index) {
    return kernel::R32(kernel::R32(node + kNodeChildFirst) + index * 4);
}

// a node found again by child indices from root, 0 when the tree no longer has the path
inline uint32_t FindNode(uint32_t root, const std::vector<int>& path) {
    uint32_t node = root;
    for (int index : path) {
        if (index < 0 || uint32_t(index) >= ChildCount(node)) return 0;
        node = ChildAt(node, uint32_t(index));
    }
    return node;
}

// a node's children and attributes traded with another's, so the node keeps its identity (an undo puts a clone's
// content back)
inline void SwapContent(uint32_t a, uint32_t b) {
    constexpr uint32_t kNodeParent = 0x44;
    constexpr uint32_t kVectorWords[] = {0x4C, 0x50, 0x54, 0x5C, 0x60, 0x64};  // children, attributes: first last end
    for (uint32_t at : kVectorWords) {
        uint32_t word = kernel::R32(a + at);
        kernel::W32(a + at, kernel::R32(b + at));
        kernel::W32(b + at, word);
    }
    for (uint32_t node : {a, b}) {
        for (uint32_t i = 0; i < ChildCount(node); i++) kernel::W32(ChildAt(node, i) + kNodeParent, node);
    }
}

// std::string: inline text until the capacity reaches 16 bytes, else a pointer
inline std::string GuestString(uint32_t str) {
    constexpr uint32_t kText = 0x04;
    constexpr uint32_t kCapacity = 0x18;
    constexpr uint32_t kInlineBytes = 0x10;
    uint32_t text = str + kText;
    if (kernel::R32(str + kCapacity) >= kInlineBytes) text = kernel::R32(text);
    return kernel::ReadCString(text);
}

// a C string into guest memory, cut to fit
inline uint32_t WriteText(uint32_t at, const std::string& text, uint32_t bytes) {
    size_t n = std::min<size_t>(text.size(), bytes - 1);
    for (size_t i = 0; i < n; i++) kernel::W8(at + uint32_t(i), uint8_t(text[i]));
    kernel::W8(at + uint32_t(n), 0);
    return at;
}

// every entity, as the editor's save walks them: the entity system's list, the entity at +0xC of a node
template <typename F>
void ForEachEntity(F&& fn) {
    constexpr uint32_t kSystemEntities = 0x24;
    constexpr uint32_t kNodeEntity = 0x0C;
    constexpr uint32_t kMaxNodes = 0x10000;  // a guard on the walk
    uint32_t system = kernel::R32(SYM_EntitySystem_pointer);
    uint32_t head = system ? kernel::R32(system + kSystemEntities) : 0;
    uint32_t node = head ? kernel::R32(head) : 0;
    for (uint32_t n = 0; node && node != head && n < kMaxNodes; n++) {
        if (uint32_t entity = kernel::R32(node + kNodeEntity)) fn(entity);
        node = kernel::R32(node);
    }
}

}  // namespace fcip::editor
