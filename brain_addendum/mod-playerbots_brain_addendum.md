# Module Brain Addendum

Module Name: mod-playerbots
Module Version: RTG Queue Assistance 2.3.7
RTG Brain Compatibility Version: 2.3.0
Commit Title: Repair BG planner startup floor and maturity handoff
Commit Description: Fixes RTG battleground planner underfilling by separating pre-start starter-fill/pop demand from mature live-refill demand. A single real queued player no longer collapses battleground helper need to 1v1; the planner now applies a minimum viable startup floor per team before handing off to live active-side balancing once the battleground is actually running.

--------------------------------

## Module Purpose

This revision repairs battleground helper demand planning so RTG can seed a real battleground startup instead of mirroring the first real queue count too literally. The planner now aims for a minimum viable per-team start size during pre-start phases, then switches to live refill/balance behavior only after active battleground participants exist.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the battleground demand-policy layer.
- Planner maturity phases are still:
  - `dormant`
  - `starter_fill`
  - `pop_or_invite`
  - `live_refill`
- This revision changes how targets are computed inside those phases.
- `RandomPlayerbotMgr` still consumes planner demand totals and performs acquisition/login/queue dispatch.
- The planner now applies a startup floor heuristic before match start and active-side balancing after match start.

--------------------------------

## Runtime Control Path

real player enters battleground queue
→ planner detects pre-start real demand
→ planner applies startup floor per team instead of mirroring 1v1
→ RTG acquires/logs in enough helpers to reach minimum viable start
→ match pops / invites form
→ active battleground players appear
→ planner hands off to `live_refill`
→ live demand now follows active in-instance side counts instead of startup fill counts

--------------------------------

## Data Structures

- BG planner global demand key
  - `rtg_bg_need_total`
- BG planner per-queue demand key
  - `rtg_bg_need_<queue>:<bracket>`
- BG planner phase key
  - `rtg_bg_phase_<queue>:<bracket>`
- Battleground snapshot counters already supplied by the manager:
  - queued real alliance/horde
  - queued current alliance/horde
  - active real alliance/horde
  - active current alliance/horde

--------------------------------

## Config Interface

Documented and used by this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.MaxRandomBots`

This revision introduces no new config keys.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `RtgBgQueuePlanner.cpp`
- `RandomPlayerbotMgr` global event cache / demand consumption
- battleground queue snapshots collected into `BattlegroundData`
- RTG runtime breadcrumb logging for planner phase/demand visibility

--------------------------------

## Lifecycle Model

- Pre-start battleground demand is now treated as a startup-seeding job.
- Mature battleground demand remains a live-refill/balance job.
- The planner no longer treats “one real player queued” as equivalent to a fully matured low-demand battleground.
- Lifecycle ownership/logout behavior is unchanged in this revision; this pass is specifically planner-policy repair.

--------------------------------

## Known Constraints

- Startup floor is currently heuristic by battleground team size:
  - team size up to 5 → floor 5
  - team size up to 15 → floor 5
  - larger battlegrounds → floor 10
- This revision intentionally avoids adding new config until the repaired phase behavior is validated.
- Further live-refill tuning may still be needed after planner repair is confirmed.

--------------------------------

## Future Evolution Hooks

- battleground-type-specific startup floors if realm design needs finer control
- explicit invite/pop phase confidence windows
- per-battleground live-refill budgets separated from startup seeding budgets
- synchronized surplus-helper collapse once startup demand is fully satisfied

--------------------------------

## Files Modified In This Revision

- `src/Bot/RtgBgQueuePlanner.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- A single real queued battleground player no longer collapses RTG helper need to 1 total helper.
- Pre-start battleground demand now aims for a minimum viable startup size per team.
- Phase breadcrumbs now include target counts so planner decisions are easier to audit in `worldserver`.
- Live battleground refill remains based on active side counts once the battleground is actually running.

--------------------------------

## Test Plan

1. Build and restart worldserver with standalone RTG queue assistance enabled.
2. Queue one real level-19 player into WSG.
3. Confirm planner logs show pre-start phase targets around 5v5 instead of `totalNeed=1`.
4. Confirm multiple helpers log in and queue, rather than only one helper.
5. Let the battleground start.
6. Confirm planner transitions to `live_refill` once active players exist.
7. Confirm mature live demand follows active battleground counts rather than starter-fill counts.

--------------------------------

## Notes For RTG Brain Ingestion

The core semantic correction in this revision is: battleground startup demand must not mirror the first observed real queue count too literally. “One real queued player” is a seed signal, not a mature target. The planner now distinguishes seed/startup demand from live mature refill demand, which aligns with the RTG brain rule that BG starter fill and BG live balance fill are separate jobs.
