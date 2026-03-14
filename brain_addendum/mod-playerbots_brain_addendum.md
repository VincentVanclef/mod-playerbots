# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8a
RTG Brain Compatibility Version: 5.4.0
Commit Title: Gate pre-start BG demand on real players and collapse orphan queue helpers
Commit Description: Prevents bot-only battleground queue residue from seeding fresh RTG startup demand and allows orphan queued RTG helpers to retire once real demand and active battleground state are both gone.

--------------------------------

## Module Purpose

This revision repairs a planner/cleanup feedback loop where lingering RTG bot queue entries could keep `pop_or_invite` alive even after the real player left queue, causing fake demand regeneration and skipped cleanup.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the authoritative BG demand producer.
- `RandomPlayerbotMgr` and `RtgQueueLifecycle` consume planner state to retain or retire helpers.
- This revision tightens the contract between planner and lifecycle by distinguishing real queued demand from bot-only queue residue.

--------------------------------

## Runtime Control Path

real player queues
→ planner sees real queued demand
→ starter fill / pop-or-invite demand is produced
→ helpers log in and queue
→ real player unqueues and no active battleground remains
→ planner drops phase to dormant instead of reusing bot-only queue counts
→ orphan queued RTG helpers lose lifecycle protection
→ helpers log out cleanly

--------------------------------

## Data Structures

- Existing BG planner event keys:
  - `rtg_bg_need_<queue>:<bracket>`
  - `rtg_bg_real_demand:<queue>:<bracket>`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`
- Existing RTG helper ledger entries in `RtgQueueLifecycle`

--------------------------------

## Config Interface

No new config entries in this revision.

Relevant existing config:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`

--------------------------------

## Database Structures

Uses existing cached reads from `world.battleground_template`. Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- existing RTG event cache consumed by `RandomPlayerbotMgr`

--------------------------------

## Lifecycle Model

Pre-start BG demand is now gated by real queued players only. Bot-only queue leftovers no longer keep `pop_or_invite` alive. If a helper remains queued with no real demand anchor and no active battleground, lifecycle retirement now treats it as an orphan and allows cleanup.

--------------------------------

## Known Constraints

- This revision focuses on pre-start orphan queue collapse, not broader live-refill redesign.
- Active battleground refill still depends on current planner/lifecycle integration already present in the branch.

--------------------------------

## Future Evolution Hooks

- Dedicated forced dequeue path for orphan queued BG helpers if branch APIs allow a cleaner direct queue removal call than logout-driven cleanup.
- Planner-side coalesced demand logging to reduce duplicate RTG lines.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- No startup demand from bot-only queued battleground leftovers.
- No fake `pop_or_invite` phase when only RTG leftovers remain in queue.
- Orphan queued RTG BG helpers can now retire once the real demand anchor disappears.

--------------------------------

## Test Plan

1. Queue solo for Eye of the Storm.
2. Confirm planner startup demand uses real queued player demand and DB-backed minima.
3. Unqueue before pop.
4. Confirm planner drops phase to dormant instead of staying in `pop_or_invite` from bot-only queue counts.
5. Confirm lingering RTG queued helpers log out instead of remaining independently queued for long periods.
6. Repeat with WSG and confirm no fake startup demand appears from leftover bot-only queue entries.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes an important RTG queue-assistance rule: **bot-created queue residue is not a legitimate pre-start demand source**. Startup demand must be anchored by real queued players, while orphan helper cleanup must be allowed once that anchor disappears.
