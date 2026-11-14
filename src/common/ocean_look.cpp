// original Xbox ocean look, title thread to render thread
#include <atomic>
#include <mutex>

#include "common/render_pass.h"

namespace {

std::mutex g_lock;
fcip::OceanLook g_look;
std::atomic<bool> g_on{false};

}  // namespace

void fcip::SetOceanLook(bool on, const OceanLook& look) {
    {
        std::lock_guard lock(g_lock);
        g_look = look;
    }
    g_on.store(on, std::memory_order_release);
}

bool fcip::OceanLookDrawn(OceanLook& out) {
    if (!g_on.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_lock);
    out = g_look;
    return true;
}
