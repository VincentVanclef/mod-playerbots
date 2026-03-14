# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8k
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore standalone RTG queue runtime and planner-first queue ticks
Commit Description: Repairs the core RTG queue-assistance runtime gate that accidentally disabled the entire standalone queue-helper system whenever RandomBotAutologin was off. The revision restores UpdateAIInternal execution for RTG event-driven mode, runs BG/LFG planner checks before helper acquisition, makes startup and acquisition breadcrumbs visible in worldserver output again, and honors planner-authored BG team-need keys when present.

--------------------------------

## Module Purpose

This revision restores the RTG queue-assistance runtime itself before further feature expansion. It ensures the standalone helper model can tick, plan, log, and acquire bots even when legacy randombot autologin is disabled.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for RTG helper planning, acquisition, login, dispatch, and retirement.
- RTG standalone mode is now explicitly allowed to run with `RandomBotAutologin = 0`.
- BG/LFG planner refresh now occurs before helper acquisition in RTG event-driven mode.
- Visible RTG breadcrumbs now emit through both `playerbots` and `server.loading` for critical control/acquisition surfaces.
- BG acquisition can consume planner-authored per-team unresolved need via `rtg_bg_team_need:<queue>:<bracket>:<team>` when available.

--------------------------------

## Runtime Control Path

server starts
→ `RandomPlayerbotMgr` runtime initializes in RTG mode
→ `UpdateAIInternal` continues ticking even with standalone helper mode
→ BG/LFG planners refresh current RTG demand first
→ helper acquisition evaluates fresh planner state
→ helpers log in with RTG queue metadata
→ immediate queue dispatch / queue grace / lifecycle ownership continue as before

--------------------------------

## Data Structures

- Existing RTG queue lifecycle / ledger structures remain authoritative.
- Additional runtime key consumed by acquisition when present:
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`

--------------------------------

## Config Interface

No new config keys were added.

Important runtime meaning reaffirmed by this revision:

- `AiPlayerbot.RTG.EventDriven.Enable = 1`
  - allows standalone RTG queue runtime to tick even when `AiPlayerbot.RandomBotAutologin = 0`
- `AiPlayerbot.RandomBotAutologin = 0`
  - no longer disables the RTG standalone queue-helper runtime
- `AiPlayerbot.RandomBotJoinBG`
- `AiPlayerbot.RandomBotJoinLfg`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

No DB schema changes.

Battleground min/max player bounds still derive from cached reads of:

- `world.battleground_template`

--------------------------------

## Integration Points

- `RandomPlayerbotMgr::UpdateAIInternal`
- `RandomPlayerbotMgr::AddRandomBots`
- `RandomPlayerbotMgr::CheckBgQueue`
- `RandomPlayerbotMgr::CheckLfgQueue`
- `RtgBgQueuePlanner`
- `RtgQueueLifecycle`
- `RtgQueueLedger`

--------------------------------

## Lifecycle Model

- Standalone RTG helpers remain event-driven and queue-owned.
- Planner refresh occurs before acquisition so helpers act on fresh demand.
- Lifecycle protections, queue grace, and safe-retire behavior remain intact.
- No return to legacy passive randombot churn.

--------------------------------

## Known Constraints

- Requires `mod-playerbots` enabled.
- Requires RTG event-driven queue assistance enabled for standalone helper mode.
- Helper supply still depends on available random bot accounts and `MaxRandomBots` headroom.
- If no real players are online, BG/LFG demand is still intentionally cleared and idle breadcrumbs are emitted.

--------------------------------

## Future Evolution Hooks

- RDF planner extraction to mirror BG planner separation
- unified RTG acquire-plan trace for BG and RDF
- queue pressure telemetry for scoreboard / dev console ingestion

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Fixes the critical runtime regression where `UpdateAIInternal` returned immediately whenever `RandomBotAutologin = 0`, which unintentionally disabled the entire RTG standalone queue-helper system.
- BG/LFG planner checks now run before acquisition during RTG event-driven ticks.
- Critical RTG breadcrumbs are now visible again in `worldserver` output through `server.loading` mirroring:
  - `[RTG][CONTROL]`
  - `[RTG][ACQUIRE][HEADROOM]`
  - `[RTG][ACQUIRE][PLAN]`
  - `[RTG][ACQUIRE][RESULT]`
  - `[RTG][ACQUIRE][MISS]`
  - `[RTG][BG][IDLE]`
  - `[RTG][LFG][IDLE]`
- BG acquisition now honors planner-authored per-team need keys when present, while preserving a fallback path.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Keep `AiPlayerbot.RTG.EventDriven.Enable = 1`.
3. Ensure `AiPlayerbot.RandomBotJoinBG = 1` and/or `AiPlayerbot.RandomBotJoinLfg = 1`.
4. Set `AiPlayerbot.MaxRandomBots` and `AiPlayerbot.RandomBotAccountCount` high enough for helper supply.
5. Rebuild and restart `worldserver`.
6. On startup, confirm a visible control breadcrumb appears:
   - `[RTG][CONTROL] queue runtime initialized ...`
7. With no real players online, confirm periodic idle breadcrumbs appear:
   - `[RTG][BG][IDLE]`
   - `[RTG][LFG][IDLE]`
8. Log in one real level-19 player and queue for BG.
9. Confirm planner breadcrumbs appear first, then acquisition breadcrumbs.
10. Confirm helper bots log in again and attempt queue dispatch.
11. Confirm mature BG ramp still acquires additional helpers when planner team-need grows.

--------------------------------

## Notes For RTG Brain Ingestion

This revision restores the queue-runtime foundation itself. The key lesson is architectural: RTG standalone helper mode must never be gated behind legacy `RandomBotAutologin` assumptions. The RTG queue system is its own runtime lane and must keep ticking, planning, and logging even when stock world randombot population is intentionally disabled.
