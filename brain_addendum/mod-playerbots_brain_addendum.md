# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8
RTG Brain Compatibility Version: 5.4.0
Commit Title: Add mature BG max-fill phase and clear queue deserter penalties
Commit Description: Extends the RTG battleground planner with a mature `bg_started` phase that escalates from battleground-specific startup minima to battleground-specific max-per-team refill after a delay, while also hardening battleground and LFG helpers against deserter-style penalties.

--------------------------------

## Module Purpose

This revision makes battlegrounds start at their true minimum viable size per `battleground_template`, then mature into fuller matches over time instead of remaining permanently at the minimum start population. It also removes deserter-style penalties from RTG-controlled helpers so they can continue assisting battleground and dungeon systems cleanly.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the source of battleground demand truth.
- Battleground startup sizing comes from cached `world.battleground_template` minima/maxima.
- Active battlegrounds now progress through a maturity ladder: `starter_fill` → `pop_or_invite` → `live_refill` → `bg_started`.
- `RandomPlayerbotMgr` consumes the planner’s per-team demand keys and existing helper lifecycle events.
- `BattleGroundJoinAction` and `LfgActions` now proactively strip deserter-style penalties from RTG helpers.

--------------------------------

## Runtime Control Path

real player queues
→ planner reads battleground-specific min/max team bounds
→ starter phase fills to `MinPlayersPerTeam`
→ battleground pops
→ early active phase balances to current active side (`live_refill`)
→ after the maturity delay the planner enters `bg_started`
→ helper demand escalates toward `MaxPlayersPerTeam` for open battlegrounds
→ helpers continue to refill active battlegrounds toward the fuller experience target
→ demand collapse clears phase/team keys and helpers retire normally

--------------------------------

## Data Structures

- `RTG_BgTemplateBounds`
- planner event keys:
  - `rtg_bg_need_<queue>:<bracket>`
  - `rtg_bg_real_demand:<queue>:<bracket>`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`
  - `rtg_bg_active_start:<queue>:<bracket>`
- existing helper event keys:
  - `rtg_bg_pending`
  - `rtg_bg_dispatch_retry`

--------------------------------

## Config Interface

No new config keys were introduced in this revision.

Relevant existing config used by this behavior:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAutologin`

`AiPlayerbot.RTG.QueueGraceSeconds` now effectively acts as the maturity delay before a started battleground escalates from minimum-start refill behavior into fuller max-team refill behavior.

--------------------------------

## Database Structures

Reads `world.battleground_template` in a cached, one-time manner for:
- `ID`
- `MinPlayersPerTeam`
- `MaxPlayersPerTeam`

Module currently uses no new DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgBgQueuePlanner`
- `BattleGroundJoinAction`
- `LfgActions`
- `BattlegroundMgr` / battleground templates
- RTG queue lifecycle and planner event cache

--------------------------------

## Lifecycle Model

BG helpers still begin life as queue-fill workers driven by planner demand. The planner now keeps them in lower-startup demand first, then increases target occupancy after the battleground has been active for the configured maturity delay. Deserter-style queue penalties are removed proactively so helpers do not get stranded or excluded from future RTG-assisted battleground or dungeon work.

--------------------------------

## Known Constraints

- Mature max-fill behavior uses the existing `AiPlayerbot.RTG.QueueGraceSeconds` delay rather than introducing a dedicated new config in this revision.
- The planner fills toward battleground template max-per-team once a battleground has been active long enough; this prioritizes fuller match experience over permanently leaving empty slots open.
- Voluntary leave prevention still relies on the existing RTG protected-helper checks in battleground actions.

--------------------------------

## Future Evolution Hooks

- Dedicated config for post-start max-fill delay separate from queue grace
- Dynamic “leave room for real players” caps by battleground population and time of day
- Explicit invite/port acceptance telemetry for helpers that miss initial battleground entry
- Mature battleground refill policy per battleground type

--------------------------------

## Files Modified In This Revision

- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`
- `src/Ai/Base/Actions/LfgActions.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Started battlegrounds no longer remain forever at `MinPlayersPerTeam`; after the maturity delay they begin requesting helpers toward `MaxPlayersPerTeam`.
- Planner phase output can now show `bg_started` to indicate mature full-fill behavior.
- RTG battleground helpers have battleground and dungeon deserter-style penalties stripped more aggressively.
- Assigned LFG helpers also clear dungeon deserter penalties before event-driven join attempts.

--------------------------------

## Test Plan

1. Start worldserver with standalone RTG battleground assistance enabled.
2. Queue solo for WSG and confirm startup still targets 5v5.
3. Let WSG start and remain active longer than `AiPlayerbot.RTG.QueueGraceSeconds`.
4. Confirm planner transitions from `live_refill` to `bg_started` and begins requesting helpers toward the battleground template max team size.
5. Repeat with EOTS and confirm startup targets 7v7, then later grows toward 15v15.
6. Force or observe a helper leave attempt and confirm deserter-style auras do not persist on RTG helpers afterward.
7. Queue an RTG-assisted dungeon/LFG helper and confirm dungeon deserter penalties are cleared before rejoin attempts.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes battleground maturity as a two-stage active lifecycle: early live refill preserves a fast startup pop, while late `bg_started` refill pursues a fuller battleground experience using authoritative battleground template maxima. It also treats deserter prevention/cleanup as part of helper lifecycle correctness for both battleground and dungeon assistance.
