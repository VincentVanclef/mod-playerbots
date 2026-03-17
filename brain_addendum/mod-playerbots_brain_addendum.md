
---

## 🧾 RTG Brain Chronicle — Dispatch Capacity Pipeline Fix

### Problem
Active bots capped (~10–13) due to dispatch stall and helpers stuck in transition.

### Fix
- Added stale recovery (45s) to reset stuck helpers
- Replaced hard stall with soft throttling (trickle allowed)
- Added pipeline visibility logs

### Result
- Continuous acquisition under pressure
- Stuck helpers recycled
- Effective capacity aligns with MaxBots


---

# 2026-03-17 — Dispatch Capacity Surgical Recovery Pass (journal append)

## Context
A prior dispatch-capacity repair attempt introduced compile regressions while trying to reopen RTG helper flow under battleground pressure. The live symptom being targeted remained the same: helper acquisition could rise initially, then flatten around roughly 10–13 active helpers with pending demand still present.

## What this pass preserves
- Existing `RandomPlayerbotMgr.cpp` structure was kept in-place.
- Existing BG/RDF queue assistance flow remains the authoritative path.
- Ownership / retirement protections remain intact.
- Adaptive level and existing RTG event-driven behavior remain intact.

## What this pass changes
1. **Pending BG lane ceiling is widened safely under real need**
   - The per-lane pending helper ceiling now scales from the standalone helper ceiling instead of acting like a near-fixed mature-lane stopgate.
   - When a bucket's unresolved need is larger than the normal lane cap, the cap can stretch upward in a bounded way instead of deadlocking finish-fill pressure behind an early threshold.

2. **Offline stale transition cleanup added**
   - Helpers stuck offline in `Reserved`, `LoggingIn`, `WorldIdle`, or `Releasing` past the dispatch stall threshold are now safely released.
   - This clears stuck `add` / queue-pending markers and removes dead reservations that were pinning capacity.

3. **Transition watchdog expanded and given recovery behavior**
   - `Reserved` and `WorldIdle` are now treated as transition states for watchdog purposes, not just `LoggingIn` / `Queued` / `Invited` / `Releasing`.
   - For BG helpers with queue-managed add-data, the watchdog first attempts a safe immediate re-dispatch back into the intended queue.
   - If recovery fails and the request has aged past the stall threshold, the helper is released cleanly so ownership and dispatch capacity can move again.
   - Pending-retire helpers still preserve their retire path instead of being force-reset into an unsafe state.

4. **Same-tick post-acquire dispatch budgeting now uses live allowed headroom**
   - The post-acquire helper dispatch recompute continues to run in the existing in-scope path.
   - Its headroom now keys from `maxAllowedBotCount` rather than the narrower raw event cap, reducing an implicit throttle during active RTG demand.

## Why this should help the 10–13 bot stall
The earlier bottleneck pattern had multiple reinforcing choke points:
- pending helpers could pile up in transition states without being reclaimed,
- mature BG lanes could stop accepting more pending helpers too early,
- and post-acquire dispatch could still be budgeted against a narrower cap than the live allowable helper budget.

This pass attacks all three while leaving already-working queue ownership and retirement behavior intact.

## Expected runtime signatures
Useful log/breadcrumb signals for validation:
- `[RTG][DISPATCH][BUDGET]`
- `[RTG][DISPATCH][POST-ACQUIRE]`
- `[RTG][RECOVER][OFFLINE_RELEASE]`
- `[RTG][RECOVER][DISPATCH]`
- `[RTG][RECOVER][RELEASE]`
- existing ownership debug lines under `RTG_QueueOwnershipDebugEnabled()`

## Validation focus
- Confirm helper counts no longer flatten around ~10–13 while BG demand still exists.
- Confirm stale helpers do not remain pinned forever in reserved/logging-in style states.
- Confirm ownership is preserved for helpers that successfully re-dispatch.
- Confirm retire-intended helpers still wait for safe release instead of being recycled aggressively.
- Confirm no regressions to existing RDF / non-RTG login behavior.
