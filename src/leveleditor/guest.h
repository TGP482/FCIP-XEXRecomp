// guest objects as the level editor reads them: methods by vtable offset, std::string, XML nodes and the entities
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

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
constexpr uint32_t kNodeNewChild = 0x5C;  // (XmlNodeRef out, tag)
constexpr uint32_t kNodeChildCount = 0x68;
constexpr uint32_t kNodeGetChild = 0x6C;
constexpr uint32_t kNodeFindChild = 0x70;
constexpr uint32_t kNodeSetAttr = 0xB0;  // (key, text)

// entity: methods, then fields
constexpr uint32_t kEntityClassName = 0x04;
constexpr uint32_t kEntityPosition = 0x10;  // (world) returns a Vec3
constexpr uint32_t kEntityId = 0xB8;
constexpr uint32_t kEntityName = 0xBC;  // std::string
constexpr uint32_t kEntityScript = 0x134;

inline uint32_t Method(uint32_t object, uint32_t slot) { return kernel::R32(kernel::R32(object) + slot); }

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
