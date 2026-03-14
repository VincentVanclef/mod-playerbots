# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8q
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore RTG queue runtime baseline and apply focused BG acquisition repair
Commit Description: Repairs the standalone RTG queue-helper runtime so logs and helper acquisition remain active with RandomBotAutologin disabled, runs planners before acquisition, restores visible RTG breadcrumbs, re-separates RTG helper cap handling from stock target drift, and applies a focused battleground per-team acquisition repair that honors planner team-need keys without re-expanding legacy randombot behavior.

--------------------------------

## Module Purpose

This revision restores the playable RTG queue-assistance baseline for battlegrounds and RDF after repeated regressions in the queue runtime control path. The purpose of this pass is not a broad rewrite. It is a focused recovery pass that preserves the live RTG event-driven architecture while repairing the exact baseline needed for logs, helper login, and planner-to-acquisition handoff.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for queue observation, helper acquisition, login, dispatch, and safe retirement.
- `RtgBgQueuePlanner` remains the RTG battleground demand planner and authoritative writer of battleground team-need event keys.
- RTG helper intent continues to flow through `add` event metadata (`rtg_bg:*` and `rtg_lfg:*`).
- RTG queue ownership and lifecycle protection remain separate from demand planning.
- Stock randombot churn behavior is not reintroduced.

--------------------------------

## Runtime Control Path

real player queue activity
→ RTG BG/RDF planners refresh demand state first
→ RTG runtime computes `rtg_target`
→ `AddRandomBots()` consumes RTG demand using RTG headroom instead of stock bot-count drift
→ helper login begins with RTG metadata attached
→ login success dispatches helper into BG or RDF flow
→ lifecycle/ownership protection keeps helper alive until queue/BG/group state is safely resolved

--------------------------------

## Data Structures

- `BattlegroundData`
- `RtgHelperLedgerEntry`
- `RtgHelperState`
- `RtgHelperOwnerType`
- `RtgHelperPurpose`
- RTG event markers used in this revision:
  - `rtg_target`
  - `rtg_bg_need_total`
  - `rtg_lfg_need_total`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_lfg_pending`

--------------------------------

## Config Interface

This revision relies on existing settings:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotJoinBG`
- `AiPlayerbot.RandomBotJoinLfg`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.MaxRandomBots`

When RTG event-driven mode is enabled, helper acquisition in this revision is again controlled by RTG runtime target state instead of drifting back to stock randombot target authority.

--------------------------------

## Database Structures

Module currently uses no DB persistence for this revision.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- battleground queue / battleground manager APIs
- LFG manager role and queue APIs
- RTG queue ownership / lifecycle helpers already present in module

--------------------------------

## Lifecycle Model

- Planners write queue demand first.
- Acquisition reads RTG demand and RTG target headroom.
- BG helper metadata is attached only inside the BG helper path.
- BG acquisition honors planner-authored per-team need when available.
- BG bucket fill alternates across unresolved team buckets instead of draining one faction first.
- No helper retirement semantics were loosened in this revision.

--------------------------------

## Known Constraints

- This revision is a focused runtime-repair pass, not the final RDF refill/state-machine pass.
- RDF initial demand and acquisition telemetry remain active, but deeper RDF refill/role-check recovery still belongs to later focused work.
- BG helper faction balance now depends on planner-authored team-need keys remaining accurate.

--------------------------------

## Future Evolution Hooks

- RDF orphan cleanup and replacement refill pass
- forced role-check and requeue role selection for RDF helpers
- extraction of more RTG queue policy out of `RandomPlayerbotMgr` and into overlay-owned queue components
- stronger queue audit telemetry for helper ownership and retirement reasons

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Standalone RTG runtime now stays alive when `AiPlayerbot.RandomBotAutologin = 0`.
- BG and RDF planners run before helper acquisition in the live update cycle.
- Critical RTG breadcrumbs are mirrored into `server.loading` as well as `playerbots`.
- RTG acquisition headroom uses `rtg_target` again instead of stock random bot count drift.
- BG helper metadata scoping is corrected so `rtg_bg_queue_grace` is only applied to BG helpers.
- BG acquisition uses planner-authored team-need keys when present and fills unresolved BG buckets round-robin across factions.

## Test Plan

1. Set `AiPlayerbot.RTG.EventDriven.Enable = 1`.
2. Set `AiPlayerbot.RTG.EventDriven.MaxBots = 60`.
3. Set `AiPlayerbot.RandomBotAutologin = 0`.
4. Keep `AiPlayerbot.RandomBotAccountCount` high enough to supply helpers.
5. Enable `AiPlayerbot.RandomBotJoinBG = 1` and `AiPlayerbot.RandomBotJoinLfg = 1`.
6. Restart `worldserver`.
7. Confirm startup breadcrumbs appear:
   - `[RTG][CONTROL] queue runtime initialized ...`
   - `[RTG][CONTROL] standalone queue-helper control active ...`
8. With no real players online, confirm idle breadcrumbs appear:
   - `[RTG][BG][IDLE] skip_check=no_real_players`
   - `[RTG][LFG][IDLE] skip_check=no_real_players`
9. Queue one real level-19 player into a battleground.
10. Confirm planner and acquisition breadcrumbs appear in order:
    - `[RTG][BG][PHASE]`
    - `[RTG][BG][DEMAND]`
    - `[RTG][ACQUIRE][HEADROOM]`
    - `[RTG][ACQUIRE][PLAN]`
    - `[RTG][ACQUIRE][RESULT]`
11. Confirm helpers no longer all spawn to one faction when both Alliance and Horde need are non-zero.
12. Queue RDF and confirm `[RTG][RDF][DEMAND]` and RDF helper acquisition still appear.

--------------------------------

## Notes For RTG Brain Ingestion

This revision should be treated as a baseline-recovery checkpoint, not a final queue-assistance milestone. The key semantic repair is that RTG queue runtime control, RTG planner timing, and RTG per-team BG acquisition are again aligned to RTG-owned event truth instead of silently drifting back to stock randombot control paths.
