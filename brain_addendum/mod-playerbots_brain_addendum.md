# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8e
RTG Brain Compatibility Version: 5.4.0
Commit Title: Honor planner team-need during mature RTG battleground fill
Commit Description: Repairs RTG battleground helper acquisition so live_refill and finish_fill planner demand is consumed from per-team incremental need events, allowing mature matches to ramp beyond minimum-start size without legacy randombot churn.

--------------------------------

## Module Purpose

This module revision closes the remaining battleground maturation gap in RTG Queue Assistance. The planner was already computing correct mature demand, but the acquisition pass still rebuilt its own battleground targets from queue visibility and absolute team-size assumptions. That caused active matches to stall after startup whenever finish-fill demand existed without fresh queue-seed visibility.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the runtime acquisition / dispatch / retirement authority.
- `RtgBgQueuePlanner` remains the single policy source for battleground phase progression and per-team helper need.
- `RtgQueueLifecycle` remains the protection layer for helper ownership and safe retirement.
- RTG battleground demand now flows planner → global team-need events → acquisition buckets without being recomputed as absolute legacy-style population targets.

--------------------------------

## Runtime Control Path

real player queues battleground
→ planner computes phase (`starter_fill`, `pop_or_invite`, `live_refill`, `finish_fill`)
→ planner writes `rtg_bg_team_need:<queue>:<bracket>:<team>` events
→ `RandomPlayerbotMgr::AddRandomBots()` builds BG acquisition buckets directly from those team-need events
→ currently active / queued / already-assigned helpers are accounted for
→ remaining incremental need is converted into additional helper logins
→ helpers immediately queue and continue through the protected RTG lifecycle

--------------------------------

## Data Structures

- `RtgBgBucket` now tracks:
  - `targetTeamCount`
  - `plannerNeed`
  - `currentTeamCount`
  - `assignedExtra`
- Existing RTG event markers remain authoritative:
  - `rtg_bg_need_total`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`

--------------------------------

## Config Interface

No new config keys were added in this revision.

This revision continues to rely on:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotAutologin`

--------------------------------

## Database Structures

No database schema changes were introduced.

Battleground sizing remains sourced from cached `world.battleground_template` planner reads and not from hardcoded values.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp` (unchanged authority surface; consumed by this revision)
- RTG BG global event cache
- existing RTG queue dispatch breadcrumbs

--------------------------------

## Lifecycle Model

- Mature battleground ramp targets are no longer inferred only from visible queue population.
- Active matches with planner-issued incremental need continue to receive additional helpers even when no fresh queue seed exists.
- Already online RTG helpers only suppress demand when they are already counted in the current team state or assigned as unresolved extra helpers for that exact planner bucket.
- Retirement behavior is unchanged and remains lifecycle-gated.

--------------------------------

## Known Constraints

- This revision assumes battleground bracket tracking in `BattlegroundData` is accurate.
- Fallback bucket construction still exists for legacy transitional cases where a managed BG helper is present before planner bucket creation.
- If `AiPlayerbot.MaxRandomBots` headroom is exhausted, mature demand will still be capped by real headroom, by design.

--------------------------------

## Future Evolution Hooks

- add dedicated acquisition breadcrumb showing plannerNeed / currentTeamCount / assignedExtra per bucket
- expose mature ramp state in scoreboard telemetry
- add queue-to-active transfer diagnostics for helpers delayed between invite and entry
- extend same planner-authoritative pattern to any future cross-BG prioritization layer

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Finish-fill and live-refill battleground demand is now acquired from planner-issued per-team incremental need.
- Active battlegrounds can continue ramping from min-per-team toward max-per-team over time.
- Acquisition no longer depends on fresh real queue visibility to continue mature-stage helper login.
- Existing pending RTG BG helpers are counted against the same planner bucket so unresolved demand is not overfilled.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Set `AiPlayerbot.MaxRandomBots` high enough to allow mature ramp steps, such as `20` or higher for the test battleground.
3. Ensure RTG event-driven queue assistance, smart queue, and queue ownership are enabled.
4. Restart `worldserver`.
5. Queue a real level-19 player into Eye of the Storm.
6. Confirm startup reaches `MinPlayersPerTeam` from `world.battleground_template`.
7. Wait through at least two mature ramp intervals.
8. Confirm logs continue to show planner demand such as `needA=2 needH=2` and that new `[RTG][BG][ACQUIRE]` logins occur after startup.
9. Confirm active population ramps approximately 7v7 → 9v9 → 11v11 → 13v13 → 15v15, subject to helper headroom.
10. Remove or lose players mid-match and confirm refill still occurs when planner demand reappears.
11. Let demand collapse and confirm helpers retire only after queue / BG / lifecycle protection clears.

--------------------------------

## Notes For RTG Brain Ingestion

The important semantic correction here is that BG acquisition now consumes planner truth instead of rebuilding its own absolute desired state from partial queue visibility. That preserves the RTG architecture split: planner computes unresolved incremental need, acquisition fulfills it, lifecycle protects it.
