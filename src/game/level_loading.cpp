// A level's loading screen stays up while the shader archive still compiles, so the level never starts with shaders
// missing
#include <cstdint>

#include "d3d/shader_archive.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kGameLoadingScreen = 0x310;  // the game's loading screen, which 82333228 draws a frame of

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Game_LoadLevel));

// (game, ...): loads a level, drawing its loading screen as it goes
PPC_FUNC(GUEST(Game_LoadLevel)) {
    uint32_t screen = R32(uint32_t(ctx.r[3]) + kGameLoadingScreen);
    GUEST_ORIG(Game_LoadLevel)(ctx, base);
    while (d3d::ShaderArchiveBusy()) CallGuest(ctx, SYM_LoadingScreen_Draw, {screen});
}
