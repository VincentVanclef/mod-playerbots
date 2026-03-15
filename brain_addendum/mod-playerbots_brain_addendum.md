# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8z
RTG Brain Compatibility Version: 5.4.0
Commit Title: Separate RDF helpers from BG dispatch and prioritize planner-driven BG refill
Commit Description: Fixes RTG service separation so helpers acquired for RDF no longer drift into battleground join behavior, RDF role realization now honors desired RTG roles when the class can perform them, and battleground refill/finish-fill buckets are prioritized ahead of startup queues using planner phase state.

--------------------------------

## Module Purpose

This revision tightens the remaining queue-assistance split between RDF and battlegrounds. RDF helpers are now protected from battleground auto-join behavior, desired RDF roles are honored during LFG role selection, and battleground refill buckets are prioritized above startup queues so active matches stop starving while fresh queues appear.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr::AddRandomBots()` now carries planner phase into BG buckets and sorts refill phases ahead of startup phases.
- `LfgJoinAction::GetRoles()` now returns the RTG desired role when the bot's class can legally perform it.
- `BGJoinAction::Execute()` now hard-blocks RTG RDF-assigned helpers from entering battleground queue logic.
- Existing runtime spine, headroom math, and dead-queue teardown behavior remain unchanged.

--------------------------------

## Runtime Control Path

real RDF demand
→ RTG LFG helper acquired with `rtg_lfg:*` add-data
→ helper login triggers RDF login/dispatch path
→ `BGJoinAction` now rejects the helper because it is LFG-assigned
→ helper remains service-isolated to RDF

real battleground demand
→ planner publishes phase/team-need
→ `AddRandomBots()` creates BG buckets with planner phase attached
→ ordered BG acquisition prioritizes `live_refill`, then `finish_fill`, then `pop_or_invite`
→ active battleground refill no longer loses all priority to a fresh queue startup

--------------------------------

## Data Structures

Relevant BG planner keys:
- `rtg_bg_phase:<queue>:<bracket>`
- `rtg_bg_team_need:<queue>:<bracket>:<team>`
- `rtg_bg_real_demand:<queue>:<bracket>`

Relevant helper metadata:
- `add`
- `rtg_lfg_pending`
- `rtg_bg_pending`
- `rtg_bg_queue_grace`

BG bucket structure now also carries:
- planner `phase`

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

No SQL or schema changes.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/LfgActions.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`

--------------------------------

## Lifecycle Model

- RDF helpers are now service-isolated from BG queue execution.
- RDF assigned role is now honored when the bot class supports that role.
- BG refill/finish-fill helpers are prioritized before startup buckets for other queues.
- Dead-queue teardown remains unchanged from the current working baseline.

--------------------------------

## Known Constraints

- This revision intentionally does not alter the runtime spine or teardown path.
- RDF still depends on the existing LFG queue engine for final acceptance; this patch ensures the helper reaches the correct service path and role selection.
- BG refill remains planner-authoritative and now gets stronger ordering priority, but runtime validation is still required to measure overlap pressure under multiple simultaneous queues.

--------------------------------

## Future Evolution Hooks

- explicit per-service helper assignment breadcrumb such as `[RTG][ASSIGN] helper=... service=lfg|bg`
- RDF owner-level fulfillment dashboards showing target vs realized roles
- refill fairness throttles between multiple active battlegrounds

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/LfgActions.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG RDF helpers no longer enter battleground queue behavior through `BGJoinAction`.
- RTG desired RDF roles are honored when the class can legally perform the role.
- BG refill and finish-fill buckets are prioritized ahead of startup queues using planner phase ordering.
- Runtime logging, headroom math, and dead-queue collapse behavior are preserved from the current baseline.

--------------------------------

## Test Plan

1. Queue one character for RDF and another for a battleground.
2. Confirm RDF helpers show `[RTG][LFG][ACQUIRE]`, `[RTG][LFG][LOGIN]`, and `[RTG][LFG][DISPATCH]` and do not subsequently queue for BG.
3. Confirm battleground startup still seeds normally.
4. Create an active battleground with missing spots, then queue a second battleground.
5. Confirm `live_refill` / `finish_fill` buckets are serviced before the fresh startup queue consumes the whole acquire wave.
6. Leave queues and confirm the improved dead-queue logout behavior remains intact.

--------------------------------

## Notes For RTG Brain Ingestion

This revision adds an important RTG queue-assistance rule: service assignment must be hard-separated at helper execution time, not only at acquisition accounting time. A helper acquired for RDF must be blocked from battleground queue execution, and battleground acquisition ordering must prefer refill/finish-fill phases over startup when multiple battleground demands coexist.
