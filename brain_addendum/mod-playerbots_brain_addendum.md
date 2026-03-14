# Module Brain Addendum

Module Name: mod-playerbots
Module Version: RTG Queue Assistance 2.3.4
RTG Brain Compatibility Version: 2.3.0
Commit Title: Surface RTG breadcrumbs and add battleground demand phase handoff
Commit Description: Routes RTG runtime breadcrumbs through a worldserver-visible log channel and upgrades battleground demand planning to use explicit maturity phases so started battlegrounds hand off from starter-fill logic to live refill logic.

--------------------------------

## Module Purpose

This module revision improves RTG queue-assistance observability and battleground maturity handling. It helps operators see RTG control-path activity in worldserver logs and makes battleground demand evolve as a battleground moves from queue formation to live-match refill behavior.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr remains the live runtime controller for RTG helper acquisition, queue dispatch, protection, and retirement.
- RtgBgQueuePlanner computes battleground helper demand and now assigns a maturity phase per battleground queue bucket.
- RTG breadcrumbs are emitted through both playerbots and server.loading log channels for better visibility.
- RTG queue ownership and lifecycle protection continue to live in the in-memory queue ledger and lifecycle helpers.

--------------------------------

## Runtime Control Path

real player battleground activity
→ RandomPlayerbotMgr updates battleground snapshots
→ RtgBgQueuePlanner computes demand phase and helper need
→ RTG helper acquisition / login runs
→ helpers queue immediately
→ planner transitions from starter fill to pop/invite to live refill as the battleground matures
→ surplus helpers can release while active-side imbalance can still request refill helpers

--------------------------------

## Data Structures

- BattlegroundInfo
  - queue and active player/bot counters used by the planner
- RTG battleground demand phase key
  - `rtg_bg_phase_<queue>:<bracket>`
- RTG global helper demand key
  - `rtg_bg_need_<queue>:<bracket>`
- RtgQueueLedger / RtgHelperLedgerEntry
  - in-memory helper ownership and lifecycle state

--------------------------------

## Config Interface

Documented and used by this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
- `AiPlayerbot.RTG.EventDriven.Debug`

This revision does not add new config keys.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- RandomPlayerbotMgr
- RtgBgQueuePlanner
- RtgQueueLifecycle
- BattlegroundMgr / Battleground templates
- worldserver logging channels (`playerbots`, `server.loading`)

--------------------------------

## Lifecycle Model

RTG battleground helpers remain lifecycle-owned in memory. This revision improves the planner side of lifecycle control by changing how helper demand is interpreted based on battleground maturity:

- starter_fill: build the first pop
- pop_or_invite: queue is live and invitations / final fill are in flight
- live_refill: battleground has active players; demand becomes refill/balance-oriented instead of template-max starter fill

--------------------------------

## Known Constraints

- Requires mod-playerbots and the RTG queue-assistance code path already present in the branch.
- Runtime breadcrumb visibility still depends on the server writing standard logging output to worldserver logs.
- Planner maturity is inferred from queue and active participant counts already collected in BattlegroundInfo.

--------------------------------

## Future Evolution Hooks

- explicit invite-window phase if the branch later exposes stronger pop-state signals
- battleground-instance-specific refill ownership
- smarter side-aware refill targeting based on deserter / disconnect / late-join patterns
- scoreboard / worldpvp integration for helper demand visualization

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG breadcrumbs are emitted to `server.loading` in addition to `playerbots`.
- Battleground planner now tracks demand maturity phases per queue bucket.
- Once a battleground has active players, demand switches to live-refill behavior and uses active-side counts instead of queue-side counts.
- Phase transitions and total battleground demand changes now emit visible RTG planner breadcrumbs.

--------------------------------

## Test Plan

1. Build and start worldserver with RTG event-driven queue assistance enabled.
2. Queue one real player into WSG.
3. Verify worldserver shows RTG planner/control breadcrumbs.
4. Confirm helpers log in and queue.
5. Allow WSG to pop.
6. Verify phase transitions appear, especially movement into `live_refill` after the battleground starts.
7. Confirm helpers outside the battleground are no longer kept alive by stale starter-fill demand.
8. Confirm started battlegrounds can still request refill demand if live-side imbalance appears.

--------------------------------

## Notes For RTG Brain Ingestion

This revision introduces the semantic rule that battleground helper demand is not static. Demand must mature with the battleground lifecycle. Starter-fill logic is appropriate before a battleground begins, but once a battleground is active the planner should hand off to refill/balance logic keyed off active participants, not queued-but-outside helpers.
