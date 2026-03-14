# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.7b
RTG Brain Compatibility Version: 4.0.0
Commit Title: Use battleground_template startup targets for RTG BG planner
Commit Description: Replaces the planner’s generic starter-floor heuristic with cached battleground-specific MinPlayersPerTeam and MaxPlayersPerTeam values loaded from world.battleground_template, so startup demand for each battleground uses the real queue requirements before handing off to live refill.

--------------------------------

## Module Purpose

Implements RTG standalone helper acquisition, queue control, protection, and retirement for playerbots, with battleground-specific startup fill and mature live-refill behavior.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr remains the live runtime controller for helper acquisition, login, queue dispatch, and release.
- RtgBgQueuePlanner computes battleground helper demand and publishes per-queue/per-bracket global demand events.
- RtgQueueLedger and lifecycle logic track ownership and retirement eligibility in memory.
- Planner maturity phases distinguish starter fill, pop/invite, and live refill behavior.

--------------------------------

## Runtime Control Path

real player queues battleground
→ RtgBgQueuePlanner reads battleground-specific startup bounds
→ planner computes starter-fill demand using battleground_template MinPlayersPerTeam / MaxPlayersPerTeam
→ RandomPlayerbotMgr acquires offline helpers
→ helpers log in and queue
→ planner transitions to live_refill when active battleground population exists
→ lifecycle releases stale or unneeded helpers when demand disappears

--------------------------------

## Data Structures

- `RTG_BgTemplateTeamBounds`
- `RTG_BgDemandPhase`
- `BattlegroundInfo`
- `RtgQueueLedgerEntry`

--------------------------------

## Config Interface

This revision does not add new config keys.

Relevant existing keys:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

Uses `world.battleground_template` as a read-only source for battleground startup sizing.

This revision loads and caches:
- `ID`
- `MinPlayersPerTeam`
- `MaxPlayersPerTeam`

The table is read once into a process-local cache via `std::call_once`, avoiding per-tick SQL churn.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `BattlegroundMgr`
- `Battleground` templates
- `WorldDatabase`
- RTG ownership/lifecycle systems

--------------------------------

## Lifecycle Model

- Starter-fill demand now honors battleground-specific startup minimums from the DB.
- Live refill still takes over once active battleground participants exist.
- Helpers remain RTG-owned in memory until lifecycle release conditions are satisfied.

--------------------------------

## Known Constraints

- Startup demand accuracy depends on `battleground_template` containing correct RTG min/max values.
- Arenas are still skipped by this planner path.
- Logging still duplicates because multiple enabled sinks/channels are active.

--------------------------------

## Future Evolution Hooks

- Cache invalidation / reload hook if battleground_template is hot-reloaded.
- Optional battleground-specific RTG override layer on top of DB values.
- Mature per-map refill policies beyond symmetric live balancing.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RtgBgQueuePlanner.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- A battleground’s startup target now comes from `battleground_template.MinPlayersPerTeam` instead of a hardcoded heuristic.
- EOTS/AB startup demand now scales to 7 per team on the user’s RTG data instead of incorrectly using WSG-style 5 per team.
- Planner breadcrumbs now show `bgTemplate`, `minPerTeam`, and `maxPerTeam` to make queue sizing decisions auditable in logs.

--------------------------------

## Test Plan

1. Build and restart worldserver.
2. Queue solo for WSG and verify planner shows `minPerTeam=5` and startup demand of 9 helpers.
3. Queue solo for Eye of the Storm and verify planner shows `minPerTeam=7` and startup demand of 13 helpers.
4. Confirm helpers log in up to the battleground-specific startup floor.
5. Confirm planner later hands off to `live_refill` once players are active inside the battleground.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes battleground startup sizing as data-driven from `world.battleground_template` instead of relying on a generic heuristic. The planner now better matches RTG’s customized low-level battleground startup thresholds while keeping SQL impact minimal through one-time cached reads.
