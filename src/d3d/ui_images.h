// pictures the UI draws with ImGui
#pragma once
#include <cstdint>

#include <imgui.h>

namespace gpu {

// render thread, inside the UI callback: a texture of w x h RGBA pixels, null once the slots run out
ImTextureID UiImage(const uint8_t* rgba, uint32_t w, uint32_t h);

}  // namespace gpu
