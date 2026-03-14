# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8p
RTG Brain Compatibility Version: 5.4.0
Commit Title: Honor planner team demand and alternate BG helper acquisition across factions
Commit Description: Fixes RTG battleground startup skew where acquisition could consume the full helper burst into a single faction. BG acquisition now reads planner-authored per-team need keys and fills BG buckets round-robin so Alliance and Horde unresolved demand are both honored during startup and mature ramp.

--------------------------------

## Module Purpose

This revision repairs a battleground acquisition skew in RTG queue assistance where helper bursts could all be consumed by one faction even when the planner was explicitly publishing unresolved need for both teams. The goal is to make startup fill and mature ramp honor planner team demand instead of accidentally starving one side.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for helper acquisition, login, dispatch, and retirement.
- `RtgBgQueuePlanner` remains the planner authority for battleground team demand.
- RTG battleground acquisition now consumes planner-authored per-team need keys first.
- BG helper login no longer drains the whole burst into the first large bucket; it iterates round-robin across queued team buckets.

--------------------------------

## Runtime Control Path

real player creates BG or RDF demand
→ planner computes unresolved RTG helper demand
→ BG planner publishes `rtg_bg_team_need:<queue>:<bracket>:<team>`
→ RTG runtime computes `rtg_target`
→ acquisition reads `rtg_target`
→ BG helper acquisition consumes unresolved per-team planner need
→ acquisition alternates across available BG buckets until burst capacity or team need is exhausted

--------------------------------

## Data Structures

- Existing RTG runtime event keys:
  - `rtg_target`
  - `rtg_lfg_need_total`
  - `rtg_bg_need_total`
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
- Existing helper metadata:
  - `add`
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_lfg_pending`

--------------------------------

## Config Interface

Primary RTG authority settings for this revision:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotAccountCount`

Important rule formalized by this revision:

- When `AiPlayerbot.RTG.EventDriven.Enable = 1`, RTG helper acquisition must not be capped by stock `bot_count` churn state or `AiPlayerbot.MaxRandomBots` randomization logic.

--------------------------------

## Database Structures

No new DB structures. Battleground minima/maxima still come from cached `world.battleground_template` reads.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- RTG planner event cache
- existing helper queue metadata / lifecycle path

--------------------------------

## Lifecycle Model

This revision does not change retirement doctrine. It changes battleground acquisition distribution so helper bursts respect unresolved planner demand on both factions instead of front-loading all helpers into the first bucket visited.

--------------------------------

## Known Constraints

- RTG standalone queue assistance still depends on helper account supply.
- If `rtg_target` is zero, no helpers will be acquired.
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots` can still reserve world bots on top of event demand according to existing RTG runtime policy.

--------------------------------

## Future Evolution Hooks

- Dedicated RDF planner buckets equivalent to mature BG team-need handling.
- Explicit queue-family fairness budget when BG and RDF both demand helpers at the same time.
- Additional BG telemetry line showing planner team need versus assigned helper count per bucket.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- BG acquisition now reads planner-authored `rtg_bg_team_need:<queue>:<bracket>:<team>` first when computing unresolved battleground need.
- BG helper burst fill now proceeds round-robin across team buckets instead of exhausting one faction bucket before the other.
- Startup fill and later mature ramp now preserve faction balance much more closely when both teams need helpers.
- Existing fallback logic still remains if a planner team-need key is absent.

## Test Plan

1. Set `AiPlayerbot.RTG.EventDriven.Enable = 1`.
2. Set `AiPlayerbot.RTG.EventDriven.MaxBots = 60`.
3. Set `AiPlayerbot.RandomBotAutologin = 0`.
4. Keep `AiPlayerbot.RandomBotAccountCount` high enough for helper supply.
5. Restart `worldserver`.
6. Queue one real level-19 player into Eye of the Storm.
7. Confirm planner shows demand on both factions, for example Alliance need plus Horde need.
8. Confirm acquisition no longer logs a whole startup burst to only one team.
9. Confirm startup fill approaches the expected split instead of `queueH=13 queueA=1` style skew.
10. Confirm later mature phases continue to add helpers to the team that still has unresolved planner need.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes another important RTG queue-assistance rule: **planner-authored battleground team demand must remain the authority for faction-side helper acquisition**. When both teams need support, helper burst distribution must not collapse into whichever bucket happens to be visited first.
