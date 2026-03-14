# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.7d
RTG Brain Compatibility Version: 4.0.0
Commit Title: Restore standalone RTG tick and planner demand visibility
Commit Description: Repaired a regression where RandomPlayerbotMgr still hard-returned when RandomBotAutologin was disabled, which prevented standalone RTG queue helpers from logging in at all. Also restored visible RTG control/demand breadcrumbs and mirrored planner battleground real-demand events onto the key consumed by battleground helper cleanup so planner state and cleanup state stay aligned.

--------------------------------

## Module Purpose

Provide RTG standalone queue assistance for battleground and dungeon helper bots inside mod-playerbots, allowing offline helpers to be acquired, logged in, queued, protected, and retired according to real realm demand instead of generic randombot churn.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr remains the live runtime orchestrator.
- RtgBgQueuePlanner computes battleground demand overlays and phase/maturity state.
- Queue metadata in add-event strings tags helpers with queue context.
- Ownership/lifecycle systems keep helpers protected while they are queued, invited, or active.
- Event cache keys coordinate planner output with acquisition and retirement logic.

--------------------------------

## Runtime Control Path

real queue demand
→ planner computes BG demand and per-team deficits
→ RandomPlayerbotMgr standalone RTG tick stays alive even with RandomBotAutologin=0
→ helper acquisition/login
→ queue dispatch
→ lifecycle ownership/protection
→ safe retirement when demand collapses

--------------------------------

## Data Structures

- `RtgBgQueuePlanner`
- `RtgBgBucket`
- `RtgLfgBucket`
- RTG event-cache keys such as:
  - `rtg_bg_need_total`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`
  - `rtg_bg_real_demand:<queue>:<bracket>`

--------------------------------

## Config Interface

Documented/used RTG queue configs include:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.RTG.EventMaxBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

Module currently uses no DB persistence for RTG queue ownership.

Planner reads world table `battleground_template` as cached reference data for startup sizing.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgBgQueuePlanner`
- `RtgQueueLifecycle`
- `RtgQueueLedger`
- `BattlegroundMgr`
- `world.battleground_template`

--------------------------------

## Lifecycle Model

Helpers are acquired from offline candidates, tagged with RTG queue metadata, protected while demand is active, and retired once queue/battleground ownership clears and planner demand falls away.

--------------------------------

## Known Constraints

- Standalone RTG helper control still depends on the live `RandomPlayerbotMgr` tick.
- `MaxRandomBots` must still be > 0 for helpers to exist online.
- Planner/cleanup coordination depends on stable event-cache keys.

--------------------------------

## Future Evolution Hooks

- stronger mature live-refill handoff
- team-aware refill recycling
- unified planner/controller tick
- tighter duplicate-breadcrumb suppression

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Standalone RTG queue-helper runtime is active again when `RandomBotAutologin = 0`.
- Worldserver-visible RTG control/demand breadcrumbs are restored.
- Battleground planner now writes the real-demand key expected by BG helper cleanup, preventing planner/cleanup desync.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`, `AiPlayerbot.MaxRandomBots > 0`, and enable RTG event-driven mode.
2. Restart worldserver and confirm `[RTG][CONTROL]` appears.
3. Queue one real player for WSG or EOTS.
4. Confirm `[RTG][BG][PHASE]`, `[RTG][BG][DEMAND]`, and `[RTG][DEMAND]` appear.
5. Confirm helpers log in again under standalone RTG control.
6. Unqueue and verify BG demand collapses back to zero in logs.

--------------------------------

## Notes For RTG Brain Ingestion

This revision is primarily a regression repair. The key lesson is that standalone RTG behavior depends on preserving the `UpdateAIInternal` live tick even when generic randombot autologin is disabled. A second critical lesson is that planner output and cleanup logic must share identical event-cache keys, otherwise helper cleanup can diverge from real planner demand even when planner math is correct.
