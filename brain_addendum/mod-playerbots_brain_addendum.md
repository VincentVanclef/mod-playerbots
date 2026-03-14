# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8b
RTG Brain Compatibility Version: 5.4.0
Commit Title: Mature BG fill ramp and orphan queue collapse
Commit Description: Refines RTG battleground planning so pre-start demand is anchored to real queued players, active battlegrounds mature from starter fill into live refill and finish-fill objectives, live incremental helper demand can spawn additional bots without being blocked by already-online RTG helpers, and orphan queued helpers are force-collapsed when real demand disappears.

--------------------------------

## Module Purpose

This revision focuses the RTG battleground helper system on three live-realm goals: reliable startup seeding, mature battleground refill/slow growth toward full matches, and clean collapse when real demand disappears.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner.cpp` now distinguishes dormant, starter-fill, pop/invite, live-refill, and finish-fill maturity states.
- `RandomPlayerbotMgr.cpp` now treats RTG BG need as incremental helper demand rather than subtracting all online RTG helpers from new refill requests.
- Orphan queued BG helpers are actively drained once planner phase becomes dormant and no real demand remains.

--------------------------------

## Runtime Control Path

real player queues
→ planner seeds starter-fill from DB-backed battleground minima
→ helpers log in and burst-queue
→ active battleground enters `live_refill`
→ after 90-second steps, planner enters `finish_fill` and grows targets by 2 per side until max team size
→ if real demand disappears and no active battleground remains, orphan queued helpers are force-left from queue and logged out

--------------------------------

## Data Structures

- `rtg_bg_need_*`
- `rtg_bg_real_demand:*`
- `rtg_bg_team_need:*`
- `rtg_bg_phase:*`
- `rtg_bg_active_start:*`

--------------------------------

## Config Interface

No new config keys in this revision.

Relevant existing keys:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAutologin`

--------------------------------

## Database Structures

Uses `world.battleground_template` as the authoritative source for `MinPlayersPerTeam` and `MaxPlayersPerTeam` via the existing cached planner lookup. Module currently uses no new DB persistence.

--------------------------------

## Integration Points

- `RtgBgQueuePlanner.cpp`
- `RandomPlayerbotMgr.cpp`
- battleground queue state inside AzerothCore battleground manager/session flow
- RTG ledger/event-cache ownership model

--------------------------------

## Lifecycle Model

- Startup helpers are seeded from real queued players only.
- Active battleground helpers move through live refill and finish-fill objectives.
- Incremental refill demand can create new helpers as long as `MaxRandomBots` headroom exists.
- Orphan queued helpers are explicitly collapsed instead of preserving fake pre-start demand.

--------------------------------

## Known Constraints

- Finish-fill uses a fixed 90-second cadence and +2-per-side growth per step in this revision.
- Refill still depends on available `MaxRandomBots` headroom when stale helpers have not yet been reclaimed.
- Logging remains intentionally verbose for RTG BG debugging.

--------------------------------

## Future Evolution Hooks

- Configurable finish-fill cadence and growth step size.
- Explicit queue-slot reclaim metrics per queue/bracket/team.
- Stronger synchronized end-of-BG cleanup bursts.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Active battlegrounds no longer log `planner=starter_vs_live_handoff` forever.
- Mature battlegrounds ramp toward full size every 90 seconds by 2 players per side until max.
- Live incremental BG need can spawn additional helpers even when many RTG helpers are already online.
- Bot-only queue residue no longer seeds fake pre-start demand, and orphan queued helpers are force-left and logged out.

--------------------------------

## Test Plan

1. Queue solo for EOTS and verify startup still seeds 7v7.
2. Join with a second real account and confirm live refill can spawn an additional helper instead of printing need without acquisition.
3. Leave the battleground queue and confirm orphan queued helpers do not keep fake `pop_or_invite` demand alive.
4. Let a battleground remain active past 90 seconds and verify planner transitions from `live_refill` to `finish_fill` with larger targets.
5. End the battleground and confirm outside stale helpers collapse more quickly.

--------------------------------

## Notes For RTG Brain Ingestion

This revision is the first focused pass that treats mature battleground support as a separate objective from startup handoff. The important semantic shift is that planner need is now interpreted as incremental team-specific refill/finish-fill work, not as a single absolute online target.
