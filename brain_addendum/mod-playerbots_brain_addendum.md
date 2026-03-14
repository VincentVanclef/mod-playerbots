# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 1.0.0
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore worldserver RTG breadcrumbs and stabilize planner demand state
Commit Description: Rebuilds the current queue-assistance pass on the uploaded mod-playerbots source by keeping the standalone RTG control path intact, routing key RTG breadcrumbs to a worldserver-visible log channel, and hardening battleground planner state cleanup so stale queue/bracket demand keys do not linger after demand disappears.

--------------------------------

## Module Purpose

Provide RTG standalone queue assistance for battleground and dungeon helper bots inside mod-playerbots, allowing offline helpers to be acquired, logged in, queued, protected, and retired according to real realm demand instead of generic randombot churn.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr remains the live runtime controller for helper login/logout and ownership handoff.
- RtgBgQueuePlanner computes battleground demand and writes event-cache keys consumed by the manager.
- Event-cache keys are the integration seam between planner math and helper lifecycle.
- BattleGroundJoinAction contains the leave-guard for protected RTG BG helpers.

--------------------------------

## Runtime Control Path

real player queues
→ CheckBgQueue refreshes BattlegroundData
→ RtgBgQueuePlanner writes phase + per-team need + global need events
→ UpdateAIInternal recalculates RTG target bot budget
→ RandomPlayerbotMgr acquires offline helpers for BG/LFG buckets
→ helper login stamps pending state and queue ownership
→ helper queues / enters BG
→ cleanup releases only when demand and ownership state allow it

--------------------------------

## Data Structures

- `RtgBgQueuePlanner`
- `RtgBgBucket`
- `RtgLfgBucket`
- event keys:
  - `rtg_bg_need_total`
  - `rtg_bg_real_demand:<queue>:<bracket>`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`

--------------------------------

## Config Interface

Used / relied on in this revision:

- `AiPlayerbot.Enabled`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotJoinBG`
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`

--------------------------------

## Database Structures

Uses read-only access to `world.battleground_template` to obtain battleground-specific startup minima/maxima.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgBgQueuePlanner`
- `BattleGroundJoinAction`
- `BattlegroundMgr`
- `WorldDatabase`
- RTG Brain 5.4.0 queue-assistance doctrine and module addendum

--------------------------------

## Lifecycle Model

- Startup demand is planner-driven and DB-backed.
- Helpers are acquired as offline candidates and brought online through the manager.
- Planner events are explicitly cleared when queue/bracket pairs disappear so stale helpers can lose protection.
- Key RTG breadcrumbs now route through a worldserver-visible channel for runtime verification.

--------------------------------

## Known Constraints

- Still depends on the mod-playerbots event-cache and acquisition path in `RandomPlayerbotMgr`.
- BG helper behavior remains sensitive to queue/bracket accounting and battleground core timing.
- This revision intentionally preserves the currently working standalone RTG runtime gate rather than redesigning the whole subsystem.

--------------------------------

## Future Evolution Hooks

- Dedicated phase transition from startup fill to invite-pop to mature live refill.
- Stronger per-helper stale classification and synchronized release waves.
- Optional cache refresh/invalidation for battleground template data if DB values change at runtime.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG control, demand, planner, and acquisition breadcrumbs are routed to `server.loading` so they show in worldserver more reliably.
- Planner now clears stale queue/bracket event keys when previously-seen battleground demand pairs disappear.
- This reduces stale demand state after unqueue / demand collapse and makes runtime diagnosis easier.

--------------------------------

## Test Plan

1. Build and restart worldserver.
2. Confirm `[RTG][CONTROL]` appears in worldserver when RTG standalone mode is enabled.
3. Queue solo for WSG and EOTS.
4. Confirm `[RTG][BG][PHASE]`, `[RTG][BG][DEMAND]`, `[RTG][DEMAND]`, and `[RTG][BG][ACQUIRE]` appear in worldserver.
5. For EOTS, verify startup uses DB-backed minimum team size and emits both per-team demand figures.
6. Unqueue and confirm planner demand collapses and stale queue/bracket keys are cleared.

--------------------------------

## Notes For RTG Brain Ingestion

This revision is intentionally conservative. It treats the uploaded mod-playerbots source as truth, preserves the known-good standalone RTG gate, and focuses on two things the user explicitly needs for safe iteration: dependable worldserver breadcrumbs and planner state cleanup when battleground demand disappears.
