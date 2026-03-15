# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8u
RTG Brain Compatibility Version: 5.4.0
Commit Title: Make BG acquisition planner-authoritative and stop phantom cross-queue need inflation
Commit Description: Repairs RTG battleground acquisition so only queues with active planner state contribute BG helper demand, preventing phantom cross-queue bucket inflation, wrong-queue helper assignment during overlapping battleground starts, and RDF starvation caused by BG-local fallback need.

--------------------------------

## Module Purpose

This revision tightens the RTG queue-assistance contract around battleground acquisition. The planner is now treated as the sole authority for which battleground queues are active and how much per-team helper demand exists. Acquisition no longer invents helper need for queues that merely appear in local player queue snapshots but do not yet have planner-authored demand.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the authoritative battleground demand producer.
- `RandomPlayerbotMgr::AddRandomBots()` now gates BG acquisition buckets behind active planner state for the specific queue/bracket pair.
- BG helper need is now planner-authored only in RTG mode; local team-size fallback no longer creates phantom demand for unrelated battleground queues.
- Existing dead-queue collapse behavior from 2.3.8t remains intact.

--------------------------------

## Runtime Control Path

real player battleground queue activity
→ `RtgBgQueuePlanner` computes phase, real-demand, and team-need keys for each active queue/bracket
→ `RandomPlayerbotMgr::AddRandomBots()` only materializes BG buckets for queue/bracket pairs with planner state present
→ helper login begins with queue-specific BG add-data metadata
→ helper dispatch remains bound to the intended battleground queue
→ dead queue collapse/retirement still tears helpers down cleanly when planner state disappears

--------------------------------

## Data Structures

Planner-authored BG event keys used by acquisition:
- `rtg_bg_real_demand:<queue>:<bracket>`
- `rtg_bg_phase:<queue>:<bracket>`
- `rtg_bg_team_need:<queue>:<bracket>:<team>`
- `rtg_bg_need_total`

Helper metadata remains unchanged:
- `add`
- `rtg_bg_pending`
- `rtg_bg_queue_grace`
- `rtg_bg_retire_when_safe`

--------------------------------

## Config Interface

No new config keys in this revision.

Relevant existing settings:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

Still uses cached battleground sizing from `world.battleground_template` through the planner path. No new schema or SQL changes.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- RTG battleground global event cache

--------------------------------

## Lifecycle Model

- BG acquisition now requires queue-specific planner state before a queue contributes helper demand.
- BG helper need in RTG mode is planner-authored only.
- Dead queue collapse and helper retirement remain queue-scoped and planner-driven.
- Active battleground demand from queue A no longer manufactures helper need for queue B.

--------------------------------

## Known Constraints

- This revision targets BG acquisition correctness during overlapping battleground demand and RDF coexistence.
- It intentionally does not change the already-working dead-queue collapse logic introduced in 2.3.8t.
- Runtime validation is still required to verify RDF demand can now surface cleanly when BG phantom demand is removed.

--------------------------------

## Future Evolution Hooks

- queue-specific helper assignment breadcrumbs such as `[RTG][BG][ASSIGN]`
- explicit RDF reservation floor while BG finish-fill is active
- queue-ownership dashboards showing assigned vs queued vs joined helpers per battleground queue

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Prevents acquisition from creating BG helper demand for queue/bracket pairs that do not currently have planner state.
- Removes local BG team-size fallback as an RTG acquisition authority in event-driven mode.
- Prevents overlapping battleground starts from inflating `totalBgNeed` with phantom queues.
- Helps preserve RDF responsiveness by removing BG-invented demand that could otherwise consume RTG helper headroom.
- Keeps the current clean dead-queue teardown behavior intact.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Enable RTG event-driven queue assistance and queue ownership.
3. Restart `worldserver`.
4. Queue only Eye of the Storm and confirm initial BG acquisition shows queue 4 helpers only.
5. Queue Warsong Gulch and Eye of the Storm at the same time and confirm helper acquisition reflects only the planner-authored need for those exact queues.
6. Confirm `totalBgNeed` no longer inflates beyond the sum of visible planner demand for the active queues.
7. Queue RDF solo and confirm RDF demand can surface without phantom BG buckets stealing headroom.
8. Leave all queues and confirm helpers still collapse and log out cleanly.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes a second critical RTG queue-assistance rule: in event-driven mode, battleground acquisition must not infer helper need from local snapshots unless the battleground planner has explicitly activated that queue/bracket pair. Planner state is the permission layer; planner team-need is the quantity layer.

## 2.3.8v — RDF materialization and no-real-player BG freeze
- Added immediate RTG LFG login/dispatch breadcrumbs and immediate `lfg join` attempt on helper login so RDF helpers are materially realized instead of only counted in acquisition totals.
- Made BG planner stop live_refill/finish_fill demand when no real queued or active players remain, while preserving orphan residue cleanup for teardown.
- Kept dead-queue collapse behavior from 2.3.8t intact.
