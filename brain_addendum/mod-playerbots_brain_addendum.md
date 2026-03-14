# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8l
RTG Brain Compatibility Version: 5.4.0
Commit Title: Separate RTG event-driven helper cap from stock MaxRandomBots
Commit Description: Fixes RTG standalone queue-helper acquisition so event-driven helper limits are controlled by AiPlayerbot.RTG.EventDriven.MaxBots and rtg_target rather than stock bot_count or MaxRandomBots-derived churn state.

--------------------------------

## Module Purpose

This revision repairs a cap-authority regression in RTG queue assistance where the live acquisition headroom could collapse to a stock randomized target such as `1` even while planner demand and RTG event caps were much higher. The goal is to keep battleground and RDF helper acquisition fully under RTG event-driven control.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for helper acquisition, login, dispatch, and retirement.
- RTG queue assistance uses planner-produced demand and `rtg_target` as the active helper ceiling.
- Stock `bot_count` / ratio randomization remains available for non-RTG behavior, but no longer overrides RTG standalone helper control.
- `AiPlayerbot.RTG.EventDriven.MaxBots` is the authority ceiling while RTG event-driven mode is enabled.

--------------------------------

## Runtime Control Path

real player creates BG or RDF demand
→ planner computes unresolved RTG helper demand
→ RTG runtime converts demand into `rtg_target`
→ acquisition reads `rtg_target`
→ helper headroom is computed against online RTG helpers
→ helpers log in until unresolved demand or RTG cap is reached

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

This revision does not change retirement doctrine. It changes cap authority at acquisition time so RTG helpers can continue to spawn up to the unresolved RTG target instead of being blocked by unrelated stock randombot limits.

--------------------------------

## Known Constraints

- RTG standalone queue assistance still depends on helper account supply.
- If `rtg_target` is zero, no helpers will be acquired.
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots` can still reserve world bots on top of event demand according to existing RTG runtime policy.

--------------------------------

## Future Evolution Hooks

- Dedicated RDF planner buckets equivalent to mature BG team-need handling.
- Separate RTG telemetry line showing `eventCap`, `rtg_target`, and `currentOnline` every acquisition tick.
- Explicit split between world reserve and queue reserve if `KeepWorldBots` is used heavily.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG startup and mature helper acquisition now use `rtg_target` for headroom in `AddRandomBots()`.
- RTG event-driven mode no longer lets stock `bot_count` randomization overwrite its effective helper ceiling.
- Startup runtime breadcrumb now reports `EventMaxBots` instead of `MaxRandomBots`.
- BG queue grace metadata is correctly scoped to BG helpers only.

## Test Plan

1. Set `AiPlayerbot.RTG.EventDriven.Enable = 1`.
2. Set `AiPlayerbot.RTG.EventDriven.MaxBots = 60`.
3. Set `AiPlayerbot.RandomBotAutologin = 0`.
4. Keep `AiPlayerbot.RandomBotAccountCount` high enough for helper supply.
5. Restart `worldserver`.
6. Queue one real level-19 player into Eye of the Storm.
7. Confirm logs show planner demand larger than 1.
8. Confirm `[RTG][ACQUIRE][HEADROOM]` now reports `maxAllowed` from RTG target logic rather than a stock value like `1`.
9. Confirm multiple helpers log in during startup until the BG reaches DB-backed minimum size.
10. Confirm later mature phases continue to ramp within RTG event cap.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes another important RTG queue-assistance rule: **event-driven helper population authority must remain local to RTG runtime control**. Stock randombot target randomization can coexist in the module, but it must not leak back into standalone RTG queue-helper headroom.
