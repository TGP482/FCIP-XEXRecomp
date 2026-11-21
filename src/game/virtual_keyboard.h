// default.xex: the original Xbox virtual keyboard page, for the title's system keyboard requests and our own entries
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip::keyboard {

// the typed text, UTF-16 in guest memory; false keeps the page open for another try
using Accept = bool (*)(PPCContext& ctx, uint32_t text, uint32_t length);

// opens the page over the current menu with a starting text and the line above it, both UTF-16 in guest memory;
// false while another entry is open
bool Open(PPCContext& ctx, uint32_t start, uint32_t info, uint32_t max_length, Accept accept);
// the line above the text, while the page is open
void SetInfo(PPCContext& ctx, uint32_t info);

}  // namespace fcip::keyboard
