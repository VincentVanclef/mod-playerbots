# Module Brain Addendum

Module Name: mod-playerbots
Module Version: RTG Queue Assistance 2.3.6
RTG Brain Compatibility Version: 2.3.0
Commit Title: Protect active BG helpers and recycle stale live-refill outsiders
Commit Description: Builds on the 2.3.4 planner/logging baseline without the 2.3.5 regression. This revision blocks RTG battleground helpers from voluntarily leaving active battlegrounds and recycles stale outside-instance helpers during live refill so freed slots can be replaced by fresh refill helpers.

--------------------------------

## Module Purpose

This revision keeps RTG battleground helpers inside live battlegrounds once they are part of the active match, while also ensuring stale outside helpers do not consume limited MaxRandomBots capacity during mature refill demand.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr remains the live runtime controller for helper acquisition, login, queue dispatch, protection, and retirement.
- RtgBgQueuePlanner continues to compute battleground demand phases, including live_refill for active matches.
- BattleGroundJoinAction now treats RTG-owned helpers as protected participants that should not voluntarily abandon active battlegrounds.
- Mature battleground refill can now recycle stale outside helpers when they stop making progress and are blocking refill capacity.

--------------------------------

## Runtime Control Path

real player queues battleground
→ planner computes starter fill / pop demand
→ helpers log in and queue
→ battleground starts and planner transitions to live_refill
→ helpers inside active BG are protected from voluntary leave
→ stale outside helpers in live_refill are recycled
→ freed slots allow replacement helpers to log in if refill demand still exists

--------------------------------

## Data Structures

- RTG battleground phase key
  - `rtg_bg_phase_<queue>:<bracket>`
- RTG battleground helper event flags
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_bg_queue_retry`
  - `rtg_bg_retire_when_safe`
- RtgQueueLedger / RtgHelperLedgerEntry
  - in-memory ownership/lifecycle tracking remains authoritative

--------------------------------

## Config Interface

Documented and used by this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.MaxRandomBots`

This revision adds no new config keys.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgBgQueuePlanner`
- `BattleGroundJoinAction` / `BGStatusAction`
- battleground runtime status and queue APIs

--------------------------------

## Lifecycle Model

- RTG helpers inside an active battleground are protected from voluntary leave until the battleground reaches `STATUS_WAIT_LEAVE`.
- RTG helpers outside the battleground during live_refill are allowed only a brief retry window.
- If such a helper is still outside and not queued once the retry window expires, it is recycled so a replacement helper can take its slot.

--------------------------------

## Known Constraints

- This revision intentionally targets battleground helper behavior only.
- Dungeon/RDF removal policy is not changed here.
- Refill quality still depends on battleground snapshots collected by the current mod-playerbots branch.

--------------------------------

## Future Evolution Hooks

- explicit battleground instance ownership for refill helpers
- synchronized end-of-match release waves
- side-priority refill based on score differential / attrition pressure

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG battleground helpers no longer voluntarily leave active battlegrounds.
- Outside stale helpers in live_refill are recycled after their retry window instead of occupying `MaxRandomBots` slots indefinitely.
- This revision is designed to preserve the successful 2.3.4 mass-login/mass-queue behavior while fixing match attrition and refill starvation.

--------------------------------

## Test Plan

1. Build and start worldserver with the 2.3.4-style standalone RTG configuration.
2. Queue one real player into WSG and confirm mass helper login/queue still happens.
3. Allow the battleground to start.
4. Confirm helpers inside the active battleground do not voluntarily leave.
5. If one helper remains outside the battleground during live_refill, confirm it is eventually recycled and a replacement can log in if demand remains.
6. Confirm end-of-match cleanup still occurs.

--------------------------------

## Notes For RTG Brain Ingestion

The key semantic rule added by this revision is that active battleground participants and stale outside-instance helpers are different lifecycle classes. Active participants should be sticky; stale outsiders should be recyclable. Treating them the same causes both match collapse and refill starvation under limited MaxRandomBots capacity.
