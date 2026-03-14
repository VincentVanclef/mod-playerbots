# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8g
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore planner-visible BG acquisition and seed RDF demand telemetry
Commit Description: Repairs the RTG queue-assistance regression that suppressed battleground helper logins and hid planner breadcrumbs. BG acquisition now consumes planner-issued per-team need keys directly so startup fill, live_refill, and finish_fill can all dispatch helpers. This revision also seeds RDF demand telemetry for the next RTG queue-assistance phase.

--------------------------------

## Module Purpose

This module revision restores the live battleground helper pipeline after the 2.3.8e/2.3.8f regression window and prepares the queue-assistance architecture for deeper RDF expansion. The intent is to keep RTG queue help fully event-driven while ensuring battleground planner demand is visible, actionable, and not reinterpreted as legacy absolute randombot population.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the authoritative battleground demand planner.
- `RandomPlayerbotMgr` now consumes planner-written `rtg_bg_team_need:<queue>:<bracket>:<team>` keys directly for BG acquisition.
- BG acquisition buckets are rebuilt from live battleground planner state rather than from only real-player queued snapshots.
- RDF/LFG remains owner-driven, but this revision adds explicit runtime RDF demand breadcrumbs so BG/RDF balancing can be evolved without losing observability.
- RTG helper metadata still flows through `add` event payloads and lifecycle gates; legacy randombot churn behavior is not restored.

--------------------------------

## Runtime Control Path

real player queue or active battleground exists
→ `RtgBgQueuePlanner` computes phase + per-team unresolved helper need
→ planner writes `rtg_bg_team_need` and `rtg_bg_need_total`
→ `RandomPlayerbotMgr` builds BG acquisition buckets from planner keys
→ offline helper candidates are acquired with `rtg_bg:*` metadata
→ helper login begins and queue grace is applied
→ immediate queue dispatch / retry path runs
→ active battleground can continue ramping during mature phases
→ helper retires only after demand vanishes and queue/BG/lifecycle gates allow it

RDF path in this revision:

real player RDF demand exists
→ `CheckLfgQueue()` computes owner-local helper role gaps
→ global helper total is published
→ `[RTG][RDF][DEMAND]` breadcrumb exposes aggregate helper pressure for future planner evolution

--------------------------------

## Data Structures

- `RtgBgBucket`
- `RtgLfgBucket`
- RTG event markers emphasized in this revision:
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_need_total`
  - `rtg_bg_start`
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_lfg_need_total`
  - `rtg_lfg_start`

--------------------------------

## Config Interface

This revision uses existing config only.

Primary settings involved:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDebug`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotAutologin`

Important semantic note:

- core RTG battleground breadcrumbs are emitted again when active planner state exists, even if the realm is not relying purely on verbose debug-only traces.

--------------------------------

## Database Structures

No schema changes.

Battleground sizing continues to come from cached battleground template data derived from the live battleground template path, not from hardcoded constants.

--------------------------------

## Integration Points

- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RandomPlayerbotMgr.cpp`
- battleground queue state in `BattlegroundData`
- RTG event cache helpers
- existing RTG lifecycle / ownership logic

--------------------------------

## Lifecycle Model

- BG helpers can now be acquired from planner-visible unresolved team need even after startup has already seeded a battleground.
- Existing active helpers still count toward reservation state, but they no longer suppress fresh mature-phase helper dispatch simply because they are online.
- Queue-grace metadata assignment is explicitly scoped to BG helpers again.
- RDF demand remains helper-role driven and visible through breadcrumbs for future queue-assistance work.

--------------------------------

## Known Constraints

- This revision does not yet introduce a dedicated RDF planner object analogous to `RtgBgQueuePlanner`; RDF remains manager-driven.
- Full compile/runtime validation still depends on the user’s AzerothCore branch and live config state.
- If `AiPlayerbot.MaxRandomBots` or account supply is too low, acquisition can still be headroom-limited even when planner demand is correct.

--------------------------------

## Future Evolution Hooks

- extract RDF planning into a dedicated RTG planner layer
- per-role RDF shortage scoring and smarter mixed BG/RDF arbitration
- scoreboard surfacing of RDF pressure and helper fill state
- richer demand-state change breadcrumbs to reduce repeated log noise without losing observability

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Restores battleground helper acquisition when planner demand exists.
- Fixes the mature-phase gap where `finish_fill` / `live_refill` need could be logged by planner logic yet never produce new helper logins.
- Moves BG acquisition source-of-truth back to planner-written per-team unresolved need.
- Re-exposes battleground phase and demand breadcrumbs during live RTG activity.
- Adds explicit `[RTG][RDF][DEMAND]` aggregate breadcrumb for next-phase RDF development.
- Re-scopes `rtg_bg_queue_grace` assignment so it is only attached to BG helper logins.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Ensure `AiPlayerbot.MaxRandomBots` is high enough for startup plus mature ramp.
3. Ensure `AiPlayerbot.RandomBotAccountCount` is high enough to supply helpers.
4. Enable RTG event-driven queue assistance and queue ownership.
5. Restart `worldserver`.
6. Queue a real level-19 player into Eye of the Storm.
7. Confirm startup helpers log in again.
8. Confirm logs show:
   - `[RTG][BG][PHASE]`
   - `[RTG][BG][DEMAND]`
   - `[RTG][ACQUIRE][HEADROOM]`
   - `[RTG][ACQUIRE][PLAN]`
   - `[RTG][QUEUE][DISPATCH]`
9. Let the battleground mature and confirm helper ramp continues beyond minimum start size.
10. Verify expected progression headroom permitting: `7v7 → 9v9 → 11v11 → 13v13 → 15v15`.
11. Queue for RDF/LFG and confirm `[RTG][RDF][DEMAND]` appears when helper demand exists.
12. Confirm helpers still retire only when queue/BG ownership and lifecycle gates permit it.

--------------------------------

## Notes For RTG Brain Ingestion

This revision should be remembered as the correction that reasserted a core RTG rule: battleground acquisition must honor planner-issued unresolved helper demand directly, not reinterpret helper need from partial live snapshots in a way that collapses mature battleground ramp behavior.
