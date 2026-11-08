// The startup movie skip. CSystem::Init plays Videos\Ubisoft.bik and the branding plate, CGame::Init plays
// Intro_FCI; both are refused at those sites. The editor boot has the logo movie and the plate but no intro movie.
// Attract mode and the Lua PlayBinkVideo binding call the player too and are left alone.
#include "common/settings.h"
#include "xdk/hle.h"

PPC_EXTERN_FUNC(GUEST_ORIG(PlayMovie));
PPC_EXTERN_FUNC(GUEST_ORIG(RaiseStartupScreen));

// Opens and plays a Bink movie. A movie it cannot open leaves the handle null and returns from the prologue, so a
// null return is the guest's own finished path and every caller copes with it.
PPC_FUNC(GUEST(PlayMovie)) {
    uint32_t from = uint32_t(ctx.lr);
    bool startup = from == SYM_UbisoftMovie_return || (SYM_IntroMovie_return && from == SYM_IntroMovie_return);
    if (!fcip::Get().skip_intros || !startup) {
        GUEST_ORIG(PlayMovie)(ctx, base);
        return;
    }
    ctx.r[3] = 0;
}

// Mode 0 loads ESRB_FCI.xbt, the UBISOFT PRESENTS plate CSystem::Init then parks for four seconds; mode 6 (the path
// without boot movies) loads loading_logo_<language>.xbt. The raise returns 1 and only writes globals its own drawing
// reads, and both call sites run once inside CSystem::Init, so every later loading screen is left alone.
PPC_FUNC(GUEST(RaiseStartupScreen)) {
    uint32_t from = uint32_t(ctx.lr);
    bool startup = from == SYM_BrandScreen_return || from == SYM_LogoScreen_return;
    if (!fcip::Get().skip_intros || !startup) {
        GUEST_ORIG(RaiseStartupScreen)(ctx, base);
        return;
    }
    ctx.r[3] = 1;
}
