# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8n
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore RTG planner-first queue runtime and activate RDF plus BG helper demand
Commit Description: Repairs the standalone RTG queue runtime so event-driven BG and RDF demand continue ticking with RandomBotAutologin disabled, runs planners before acquisition, restores always-on RTG breadcrumbs, and makes BG acquisition consume planner-authored per-team need while RDF demand emits and acquires helpers from the same RTG cap authority.

--------------------------------

## Module Purpose

This revision is a runtime recovery and alignment pass for RTG queue assistance. Its purpose is to stop the repeated regressions where planner logs disappear, acquisition goes silent, or helper login collapses back into stock randombot behavior. The system now keeps BG and RDF demand under the same RTG event-driven runtime and planner-first scheduling path.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for helper acquisition, login, dispatch, and retirement.
- RTG event-driven mode is allowed to tick even when `AiPlayerbot.RandomBotAutologin = 0`.
- BG and RDF planners execute before acquisition in the manager update loop.
- BG acquisition now consumes planner-authored `rtg_bg_team_need:<queue>:<bracket>:<team>` keys as its first authority.
- RDF/LFG demand continues to build owner-local role demand and a capped global helper total, and now emits explicit RDF telemetry breadcrumbs for shared queue visibility.
- RTG runtime breadcrumbs are mirrored to both `playerbots` and `server.loading` so worldserver observability survives logger filter differences.

--------------------------------

## Runtime Control Path

real player creates BG and/or RDF demand
→ planner refresh runs first in `UpdateAIInternal`
→ BG planner writes per-team need and total need
→ RDF planner writes owner-local role demand and total helper need
→ RTG runtime computes `rtg_target`
→ acquisition reads fresh planner state
→ helpers log in for RDF and BG until unresolved demand or RTG event cap is reached

--------------------------------

## Data Structures

- Existing RTG runtime event keys:
  - `rtg_target`
  - `rtg_lfg_need_total`
  - `rtg_lfg_start`
  - `rtg_bg_need_total`
  - `rtg_bg_start`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
- Existing helper metadata:
  - `add`
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_lfg_pending`
- Existing RTG queue ownership / lifecycle data remains unchanged by this revision.

--------------------------------

## Config Interface

Primary RTG authority settings for this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotJoinBG`
- `AiPlayerbot.RandomBotJoinLfg`

Important rules reinforced by this revision:

- RTG standalone queue assistance must continue running with `AiPlayerbot.RandomBotAutologin = 0`.
- BG and RDF planner refresh must happen before helper acquisition.
- `AiPlayerbot.RTG.EventDriven.MaxBots` remains the effective RTG queue-helper cap while RTG event-driven mode is enabled.

--------------------------------

## Database Structures

No new DB structures. Battleground minima and maxima still come from cached `world.battleground_template` reads inside the BG planner layer.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- RTG planner event cache
- existing helper queue metadata / lifecycle path

--------------------------------

## Lifecycle Model

This revision does not revert lifecycle protections. Helpers still remain RTG-owned and only retire through existing lifecycle gates. The key change is runtime scheduling and acquisition authority: fresh BG and RDF planner demand is now visible before acquisition each tick, so helpers can actually be logged in for both services when demand exists.

--------------------------------

## Known Constraints

- Helper acquisition still depends on offline character supply across configured bot accounts.
- If the planner writes zero demand, no helpers will be acquired.
- If `AiPlayerbot.RTG.EventDriven.KeepWorldBots` is enabled, world reserve still stacks on top of queue demand according to existing RTG runtime policy.

--------------------------------

## Future Evolution Hooks

- Extract RDF planning into a dedicated RTG overlay file parallel to `RtgBgQueuePlanner`.
- Add explicit per-service acquisition telemetry showing allocated RDF vs BG capacity on every RTG acquisition cycle.
- Continue reducing stock-file branching by moving more RTG queue logic behind thin hook calls.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG standalone queue runtime now continues ticking with `RandomBotAutologin = 0`.
- BG and RDF planner checks now run before `AddRandomBots()`.
- BG acquisition now honors planner-authored per-team need keys before falling back to local team-size inference.
- RDF demand now emits explicit `[RTG][RDF][DEMAND]` breadcrumbs alongside existing LFG planning.
- Critical RTG runtime breadcrumbs now show through both `playerbots` and `server.loading`.
- Startup and idle runtime breadcrumbs are restored for worldserver visibility.

## Test Plan

1. Set `AiPlayerbot.RTG.EventDriven.Enable = 1`.
2. Set `AiPlayerbot.RTG.EventDriven.MaxBots = 60`.
3. Set `AiPlayerbot.RandomBotAutologin = 0`.
4. Keep `AiPlayerbot.RandomBotJoinBG = 1` and `AiPlayerbot.RandomBotJoinLfg = 1`.
5. Keep `AiPlayerbot.RandomBotAccountCount` high enough for helper supply.
6. Restart `worldserver`.
7. Confirm startup shows `[RTG][CONTROL] queue runtime initialized ...`.
8. With no real players online, confirm `[RTG][BG][IDLE]` and `[RTG][LFG][IDLE]` appear.
9. Queue one real level-19 player into a battleground and confirm `[RTG][BG][PHASE]`, `[RTG][BG][DEMAND]`, `[RTG][ACQUIRE][HEADROOM]`, and `[RTG][QUEUE][DISPATCH]` appear.
10. Queue RDF/LFG demand and confirm `[RTG][RDF][DEMAND]` appears and helpers log in for dungeon roles.
11. Confirm BG startup fill occurs, mature BG ramp can continue, RDF helper roles fill, and shared RTG cap still limits combined helper population cleanly.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes a high-priority RTG queue-assistance doctrine point: **planner-first runtime scheduling is not optional**. When RTG is running standalone, BG and RDF demand must be refreshed before acquisition and must remain observable in worldserver logs, or the entire queue-assistance layer appears dead even when the planner logic itself is correct.
