# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.7c
RTG Brain Compatibility Version: 4.0.0
Commit Title: Make RTG BG planner team-aware with DB-backed startup minima
Commit Description: Repair battleground startup and cleanup behavior by driving per-team helper demand from battleground_template minima/maxima and planner-side team deficit events, then consuming those events in RandomPlayerbotMgr acquisition and stale-helper release.

--------------------------------

## Module Purpose

This revision improves RTG battleground queue assistance so helper acquisition respects battleground-specific minimum team sizes and actual per-team deficits instead of collapsing demand into a single undifferentiated total.

--------------------------------

## Architecture Overview

- RtgBgQueuePlanner computes battleground demand from BattlegroundData.
- battleground_template is loaded once and cached for MinPlayersPerTeam / MaxPlayersPerTeam.
- Planner now emits per-team deficit events in addition to global demand.
- RandomPlayerbotMgr acquisition consumes planner-side per-team deficits for helper login decisions.
- RandomPlayerbotMgr cleanup uses per-team deficit state to release stale helpers when demand disappears.

--------------------------------

## Runtime Control Path

real player queues battleground
→ RtgBgQueuePlanner loads template bounds from battleground_template cache
→ planner computes phase and per-team targets
→ planner writes rtg_bg_team_need:<queue>:<bracket>:<team>
→ RandomPlayerbotMgr builds BG buckets from per-team planner need
→ helpers log in for the exact needed team
→ if queue demand collapses, cleanup sees team need = 0 and releases stale outside helpers

--------------------------------

## Data Structures

- RTG_BgTemplateBounds
- RtgBgBucket
- BattlegroundInfo
- CachedEvent / BotEventCache

--------------------------------

## Config Interface

This revision uses existing RTG queue-assistance config only.

Key entries involved:
- AiPlayerbot.RTG.EventDriven.Enable
- AiPlayerbot.RTG.SmartQueue.Enable
- AiPlayerbot.RTG.QueueGraceSeconds
- AiPlayerbot.RTG.QueueOwnership.Enable
- AiPlayerbot.RandomBotAutologin
- AiPlayerbot.MaxRandomBots
- AiPlayerbot.RandomBotAccountCount

--------------------------------

## Database Structures

Reads world.battleground_template:
- ID
- MinPlayersPerTeam
- MaxPlayersPerTeam

Module currently uses no new DB persistence.

--------------------------------

## Integration Points

- RandomPlayerbotMgr
- RtgBgQueuePlanner
- BattlegroundMgr / Battleground templates
- WorldDatabase (read-only cached query)
- RTG queue ownership / lifecycle cleanup

--------------------------------

## Lifecycle Model

- Startup demand now stays team-aware.
- Pending helpers subtract from planner need before more bots are acquired.
- When per-team need collapses to zero, stale outside helpers are no longer protected by old pending state.

--------------------------------

## Known Constraints

- Planner events are bracket-scoped; acquisition maps them through the player queue level used for the request.
- Cache load assumes battleground_template remains stable during runtime.
- This pass focuses on battleground startup/refill and demand collapse, not dungeon helper policy.

--------------------------------

## Future Evolution Hooks

- Move per-team target data into a dedicated planner state structure instead of event-cache strings.
- Add explicit invite-stage accounting if battleground invite counts become available.
- Unify duplicate RTG breadcrumb channels to avoid duplicate log lines.

--------------------------------

## Files Modified In This Revision

- src/Bot/RtgBgQueuePlanner.cpp
- src/Bot/RandomPlayerbotMgr.cpp
- brain_addendum/mod-playerbots_brain_addendum.md

--------------------------------

## Behavioral Changes In This Revision

- Eye of the Storm startup now uses battleground_template minima (7 per team) instead of a WSG-like generic floor.
- Helper acquisition now honors per-team deficit instead of dumping the entire login burst onto the larger deficit team because of MaxPlayersPerTeam math.
- Unqueuing or demand collapse now clears stale BG helper protection more aggressively when team need drops to zero.

--------------------------------

## Test Plan

1. Queue solo for WSG and verify startup targets are 5v5 and helper team split is balanced.
2. Queue solo for Eye of the Storm and verify startup targets are 7v7 and helper split becomes 6 Alliance / 7 Horde when one real Alliance is queued.
3. Unqueue before pop and verify stale outside helpers are released instead of lingering online.
4. Let a battleground start, then observe that mature cleanup still respects active in-BG helpers while outside stale helpers can be retired.

--------------------------------

## Notes For RTG Brain Ingestion

This revision establishes a stronger architectural rule for RTG battleground assistance: planner demand must be expressed per team and derived from authoritative battleground_template startup bounds, while RandomPlayerbotMgr acquisition should consume planner output rather than rebuilding battleground sizing from raw max-team assumptions.
