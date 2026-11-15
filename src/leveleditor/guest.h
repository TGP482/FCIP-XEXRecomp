// guest objects as the level editor reads them: a method by its vtable offset, a std::string's text
#pragma once
#include <cstdint>
#include <string>

#include "kernel/kernel.h"

namespace fcip::editor {

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

}  // namespace fcip::editor
