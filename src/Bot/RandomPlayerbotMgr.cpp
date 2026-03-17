// RTG ARENA LANE PATCH (SAFE ADDITIVE)

#include "RandomPlayerbotMgr.h"

// ==============================
// RTG ARENA HELPERS (ISOLATED)
// ==============================

static uint32 RTG_ComputeArenaBotLevel(uint32 minL, uint32 maxL, uint32 avgL)
{
    int spread = (rand() % 7) - 3; // -3 to +3
    int lvl = int(avgL) + spread;

    if (lvl < int(minL)) lvl = minL;
    if (lvl > int(maxL)) lvl = maxL;

    return uint32(lvl);
}

// NOTE:
// This is intentionally minimal + non-invasive.
// Hook point: call this from existing RTG demand loop WITHOUT touching BG/RDF.

// Example integration point (pseudo-safe):
// if (rtgArenaEnabled)
//     HandleRTGArenaDemand();

void RandomPlayerbotMgr::HandleRTGArenaDemand()
{
    // SAFETY: do nothing if disabled (config to be wired later)
    // Placeholder minimal safe integration

    // Example debug
    LOG_INFO("server.loading", "[RTG][ARENA] Arena lane active (stub)");
}
