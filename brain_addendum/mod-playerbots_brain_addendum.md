# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8c
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore full RTG startup burst login pacing
Commit Description: Removes the legacy randomBotsPerInterval throttle from event-driven RTG helper acquisition so battleground startup and live refill requests can burst-log the full computed helper count instead of stalling at the stock per-interval randombot cap.

--------------------------------

## Module Purpose

Provide playerbot runtime systems for RTG queue assistance, including battleground helper acquisition, queue dispatch, lifecycle protection, refill, and retirement.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr owns the live helper acquisition/login/logout loop.
- RtgBgQueuePlanner computes battleground demand and phase/maturity targets.
- Event cache keys bridge planner output into live acquisition and cleanup.
- Queue ownership and lifecycle protection keep RTG helpers alive while they are still needed.

--------------------------------

## Runtime Control Path

real player queue demand
→ planner computes BG need
→ RandomPlayerbotMgr computes interval helper add budget
→ helper acquisition/login
→ immediate BG queue dispatch
→ active BG refill / retirement

--------------------------------

## Data Structures

- RtgBgBucket
- RtgLfgBucket
- RTG event cache keys such as `rtg_bg_need_total`, `rtg_bg_team_need:*`, and `rtg_bg_phase:*`

--------------------------------

## Config Interface

Uses the existing RTG/playerbots config set, especially:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- legacy `AiPlayerbot.RandomBotsPerInterval` remains relevant for non-RTG legacy randombot churn, but no longer throttles RTG event-driven helper bursts.

--------------------------------

## Database Structures

Uses `world.battleground_template` through the planner cache for DB-backed startup minima/maxima.

--------------------------------

## Integration Points

- RandomPlayerbotMgr
- RtgBgQueuePlanner
- Battleground queue state in playerbots/AzerothCore

--------------------------------

## Lifecycle Model

RTG helpers are acquired from offline candidates, logged in, queued, protected while needed, then retired when planner demand and runtime ownership both collapse.

--------------------------------

## Known Constraints

- Requires RTG event-driven mode to be enabled.
- Still depends on `MaxRandomBots` headroom and available helper characters/accounts.
- This revision focuses on acquisition pacing, not final orphan cleanup.

--------------------------------

## Future Evolution Hooks

- Phase-aware finish-fill ramps
- Better synchronized end-of-BG mass cleanup
- More explicit orphan-queue collapse breadcrumbs

--------------------------------

## Files Modified In This Revision

- src/Bot/RandomPlayerbotMgr.cpp
- brain_addendum/mod-playerbots_brain_addendum.md

--------------------------------

## Behavioral Changes In This Revision

RTG event-driven BG demand is no longer capped by `RandomBotsPerInterval`, so minimum-start and refill bursts can bring in the full computed helper count within the current interval.

--------------------------------

## Test Plan

1. Queue solo for EOTS.
2. Confirm `bgNeed=13` no longer results in only 3 helpers logging in.
3. Confirm logs now show `[RTG][ACQUIRE][BURST] requested=13 cap=13 reason=event_driven` (or equivalent current need).
4. Verify helpers log in and queue in a synchronized burst.
5. Repeat with WSG and with a later live-refill increment such as `needH=1`.

--------------------------------

## Notes For RTG Brain Ingestion

The key regression fixed here was legacy stock `RandomBotsPerInterval` throttling still being applied to RTG event-driven helper acquisition. That made planner demand look correct while actual helper login stalled at the old stock per-interval cap.
