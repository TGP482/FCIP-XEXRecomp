#pragma once
#include <cstdint>

// The title's CPU reads of resolves: copies made at each resolve to an address the CPU reads, or written to guest
// memory by the render thread in command order (Backend::Copy)
namespace gpu {

// a resolve the title issued to base
void NoteResolve(uint32_t base);
// a fetch (host order, GPU address) of memory the title resolved to
bool IsResolve(const uint32_t fetch[6]);
// a lock of a texture (fetch as above): guest memory gets its last resolve, false when no copy of it was made
bool ApplyReadBack(const uint32_t fetch[6]);
// numbers the title's Copy calls, whose destination (fetch as above) holds no resolve from then on: call it with Copy
// under the lock that orders them
uint64_t QueueCopy(const uint32_t dst[6]);
// until the render thread ran Copy n
void WaitCopy(uint64_t n);

}  // namespace gpu
