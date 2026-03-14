# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.3
RTG Brain Compatibility Version: 2.3.0
Commit Title: Collapse surplus BG helpers after match start
Commit Description: Updates RTG battleground demand planning so an already-started battleground no longer keeps demanding bots up to the stock template max team size. Active-match demand now balances to the larger currently active side, allowing surplus helpers that never entered the battleground to release cleanly instead of idling online outside the match.

--------------------------------

## Module Purpose

Provides playerbot systems for RTG queue assistance, helper acquisition, battleground support, and lifecycle control.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr drives live helper login, queueing, and retirement.
- RtgBgQueuePlanner computes battleground demand and writes temporary RTG event-cache targets.
- RtgQueueLedger tracks helper ownership and lifecycle state in memory.
- RtgQueueLifecycle decides whether owned helpers may retire safely.

--------------------------------

## Runtime Control Path

real queue demand
→ RTG battleground planner computes unresolved helper need
→ RandomPlayerbotMgr acquires and logs helper bots
→ helpers queue / enter battleground
→ planner recomputes need after match start
→ surplus non-participating helpers lose demand protection
→ lifecycle release path can retire them

--------------------------------

## Data Structures

- BattlegroundInfo
- RtgHelperLedgerEntry
- RtgBgTargetKey
- RtgQueueLedger

--------------------------------

## Config Interface

Documented configs used by this revision include:

- AiPlayerbot.RTG.EventDriven.Enable
- AiPlayerbot.RTG.SmartQueue.Enable
- AiPlayerbot.RTG.EventDriven.MaxBots
- AiPlayerbot.RTG.EventDriven.KeepWorldBots
- AiPlayerbot.RTG.QueueGraceSeconds
- AiPlayerbot.RTG.QueueOwnership.Enable

This revision does not introduce new config keys.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- RandomPlayerbotMgr
- BattlegroundMgr / battleground templates
- Battleground queue state stored in BattlegroundInfo
- RTG in-memory queue ledger / lifecycle systems

--------------------------------

## Lifecycle Model

Helpers are still acquired through RTG demand and protected during queue/BG ownership. This revision changes only how battleground demand collapses after an instance has already started, so excess helpers outside the match are no longer kept alive by a stale full-team target.

--------------------------------

## Known Constraints

- Assumes BattlegroundInfo active and queued counts are accurate.
- Does not add persistent per-instance demand state.
- Uses active-side balance instead of stock template max once a BG has started.

--------------------------------

## Future Evolution Hooks

- Instance-aware desired team-size caps for small-pop RTG battlegrounds
- Better release breadcrumbs for surplus helper retirement
- Dedicated live-balance rules for mid-match replacement helpers

--------------------------------

## Files Modified In This Revision

- src/Bot/RtgBgQueuePlanner.cpp
- brain_addendum/mod-playerbots_brain_addendum.md

--------------------------------

## Behavioral Changes In This Revision

- Started battlegrounds no longer keep RTG bg demand pegged to the stock template max team size.
- Surplus helpers that logged in for a queue burst but never entered the battleground should now become releasable once the active match is balanced.

--------------------------------

## Test Plan

1. Start worldserver with RTG event-driven queue assistance enabled.
2. Queue a single real player into WSG.
3. Let helpers log in and allow the battleground to pop.
4. Confirm only the helpers that actually entered the battleground remain protected.
5. Confirm surplus helpers outside the battleground begin releasing instead of staying online indefinitely.
6. Repeat once with a bot leaving mid-match to verify the planner still leaves room to rebalance the smaller active side.

--------------------------------

## Notes For RTG Brain Ingestion

This revision changes the battleground demand planner rather than the login/queue code. The key semantic shift is that active battleground balancing is now based on currently active side counts instead of the battleground template's full max team size. This prevents stale post-pop demand from keeping surplus RTG helpers online outside the started match.
