# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8r
RTG Brain Compatibility Version: 5.4.0
Commit Title: Treat RTG planner demand as incremental helper headroom and block stale helper requeue
Commit Description: Fixes RTG event-driven helper acquisition so battleground and RDF planner demand is interpreted as additional unresolved helper demand bounded by AiPlayerbot.RTG.EventDriven.MaxBots rather than as an absolute online-helper ceiling. Also blocks stale assigned BG/RDF helpers from re-queueing after demand has ended so they can retire cleanly.

--------------------------------

## Module Purpose

This revision completes a focused RTG queue-assistance repair pass aimed at the remaining gameplay blockers after runtime restoration. It fixes the acquisition handoff so finish-fill and RDF coexistence can spawn additional helpers beyond already-online startup helpers, and it prevents stale assigned helpers from autonomously rejoining battleground or RDF queues after the event/run has ended.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live RTG runtime manager for planner refresh, acquisition, dispatch, and retirement.
- RTG acquisition headroom now separates **event cap ceiling** from **unresolved planner demand**.
- `BattleGroundJoinAction` and `LfgJoinAction` now validate that assigned RTG helpers still have active planner demand before queueing.
- Existing BG round-robin team fill, planner phases, and lifecycle safety remain intact.

--------------------------------

## Runtime Control Path

real-player BG/RDF demand
→ planner computes unresolved helper need
→ acquisition computes `freeCap = EventMaxBots - managedOnlineHelpers`
→ acquisition computes `headroom = min(unresolvedNeed, freeCap)`
→ helper buckets fill BG/RDF demand
→ when demand ends, assigned helpers are blocked from re-queueing and can retire safely

--------------------------------

## Data Structures

Active RTG global demand keys used by this revision:

- `rtg_bg_need_total`
- `rtg_lfg_need_total`
- `rtg_bg_demand:<queue>:<bracket>`
- `rtg_bg_team_need:<queue>:<bracket>:<team>`
- `rtg_target`

Active helper metadata relied on by this revision:

- `add`
- `rtg_bg_pending`
- `rtg_bg_queue_grace`
- `rtg_bg_retire_when_safe`
- `rtg_lfg_pending`
- `rtg_dungeon_active`

--------------------------------

## Config Interface

Primary RTG authority settings for this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotAccountCount`

Formalized rules in this revision:

- In RTG mode, planner need is **incremental unresolved demand**.
- In RTG mode, `AiPlayerbot.RTG.EventDriven.MaxBots` is the helper ceiling.
- Assigned BG/RDF helpers must not queue unless their planner demand is still active.

--------------------------------

## Database Structures

No new DB structures. Battleground min/max sizing continues to come from cached `world.battleground_template` reads through the existing planner flow.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `src/Ai/Base/Actions/LfgActions.cpp`
- RTG planner event cache
- existing helper queue lifecycle / retirement path

--------------------------------

## Lifecycle Model

This revision preserves the RTG lifecycle doctrine:

- helpers do not voluntarily leave live battlegrounds
- helpers do not keep deserter penalties
- helpers only retire when out of queue/BG/LFG danger

New behavior added by this revision:

- stale assigned BG helpers are blocked from re-queueing when their specific queue/team demand no longer exists
- stale assigned RDF helpers are blocked from re-queueing when owner/global RDF demand has ended

--------------------------------

## Known Constraints

- Retirement still depends on the existing safe logout path and lifecycle gates.
- `currentBots` still defines the current RTG-managed helper population used for acquisition headroom.
- RDF refill/orphan handling beyond queue re-entry blocking remains part of the next hardening pass if edge cases remain.

--------------------------------

## Future Evolution Hooks

- Dedicated RTG helper planner class to isolate demand aggregation from `RandomPlayerbotMgr`.
- Explicit telemetry for `managedOnlineHelpers` distinct from `currentBots` if further precision is needed.
- Stronger RDF orphan replacement accounting once role-check/requeue behavior is fully hardened.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `src/Ai/Base/Actions/LfgActions.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG acquisition headroom now uses event-cap math instead of treating planner need as an absolute online target.
- Finish-fill and RDF coexistence can now request additional helpers even when startup helpers are already online.
- BG assigned helpers only queue if their exact planner queue/team/bracket still has demand.
- RDF assigned helpers only queue if owner/global RDF demand still exists.
- Stale helpers stop re-queueing themselves after BG or RDF demand ends, allowing clean retirement.
- Standalone RTG runtime remains enabled even when `AiPlayerbot.RandomBotAutologin = 0`.

## Test Plan

1. Set `AiPlayerbot.RTG.EventDriven.Enable = 1`.
2. Set `AiPlayerbot.RTG.EventDriven.MaxBots = 60`.
3. Set `AiPlayerbot.RandomBotAutologin = 0`.
4. Keep `AiPlayerbot.RandomBotAccountCount` high enough for helper supply.
5. Restart `worldserver`.
6. Queue one real level-19 player into Eye of the Storm and let startup seed complete.
7. Wait for `finish_fill` and confirm headroom logs now show event-cap math, for example `eventCap=60 freeCap=47 headroom=16` instead of shrinking to `3` when `13` helpers are already online.
8. Queue RDF while BG helpers are online and confirm RDF demand can still acquire helpers from remaining free cap.
9. Let an EotS match end and confirm former BG helpers do not immediately requeue if no fresh planner demand exists.
10. Let an RDF run end and confirm former dungeon helpers do not immediately requeue if no owner/global RDF demand exists.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes a critical RTG queue doctrine rule: **planner need is additive unresolved helper demand, not the desired total number of online helpers**. It also formalizes that helper assignment alone is not enough to justify queue entry; active planner demand must still exist for the helper’s BG/RDF context.

================================
## Revision: 2.3.8s Focused BG Acquire Stabilization

### Purpose
This repair pass is intentionally narrow. It corrects a regression where BG helper acquisition could collapse back into one-sided faction fills and where critical RTG acquisition breadcrumbs could disappear from the worldserver-visible log stream.

### Key Corrections
- RTG runtime breadcrumbs are now mirrored into `server.loading` in addition to `playerbots`.
- BG acquisition now prefers planner-authored `rtg_bg_team_need:<queue>:<bracket>:<team>` keys instead of relying only on local `teamSize - currentTeamCount` reconstruction.
- BG acquisition now fills unresolved team buckets in a round-robin loop instead of draining one bucket fully before attempting the opposite faction.
- RTG acquisition headroom, plan, result, and miss breadcrumbs are emitted through the shared runtime breadcrumb path so worldserver-visible diagnosis stays intact.

### Doctrine Reminder
For RTG BG queue assistance, the planner is authoritative for per-team unresolved need. Acquisition must honor that planner output and distribute helper logins across factions without allowing one bucket to consume the whole burst opportunistically.

### Files Modified
- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`
