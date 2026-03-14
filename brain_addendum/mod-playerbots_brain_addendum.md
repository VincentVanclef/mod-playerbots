# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8j
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore planner-first RTG ticks and always-on queue breadcrumbs
Commit Description: Repairs RTG queue assistance by running BG/RDF planner refresh before helper acquisition, restoring always-on RTG demand and acquisition breadcrumbs, and keeping battleground planner phase/demand logs visible without requiring debug-only flags.

--------------------------------

## Module Purpose

This module revision strengthens RTG battleground queue assistance so helper bots behave like explicit queue workers rather than passive randombots. The revision focuses on the post-login helper lifecycle: login, immediate queue attempt, queue retry protection, and orderly retirement when battleground demand disappears.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live runtime manager for helper acquisition, login, queue dispatch, and retirement.
- RTG helper intent is still encoded through `add` event metadata (`rtg_bg:*` and `rtg_lfg:*`).
- `RtgQueueLifecycle` remains the in-memory ownership and retirement gate layer.
- Helper state is protected through queue-grace event markers and ledger protection windows.
- Immediate queue dispatch is performed directly from the live manager once a BG helper reaches login success.

--------------------------------

## Runtime Control Path

real player queues battleground
→ RTG demand planner raises BG demand events
→ `RandomPlayerbotMgr` acquires offline helper candidates
→ helper login begins with RTG queue metadata attached
→ `OnPlayerLogin` marks helper as RTG-owned BG helper
→ immediate BG queue dispatch is attempted
→ queue retry/grace protection keeps helper alive while queueing
→ helper becomes queued / invited / in battleground
→ helper retires only when demand is gone and lifecycle gate allows release

--------------------------------

## Data Structures

- `RtgHelperLedgerEntry`
- `RtgHelperState`
- `RtgHelperOwnerType`
- `RtgHelperPurpose`
- RTG event markers used in this revision:
  - `rtg_bg_pending`
  - `rtg_bg_queue_grace`
  - `rtg_bg_queue_retry`
  - `rtg_bg_retire_when_safe`
  - `rtg_lfg_pending`
  - `rtg_dungeon_active`

--------------------------------

## Config Interface

This revision relies on existing RTG/playerbot settings:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotAutologin`

`AiPlayerbot.RTG.QueueGraceSeconds` now also acts as the post-login battleground queue-dispatch grace window for RTG helpers.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `PlayerbotAI`
- battleground queue / battleground manager APIs
- `RtgQueueLifecycle`
- `RtgQueueLedger`
- playerbot BG join action (`bg join`)

--------------------------------

## Lifecycle Model

- Helper is acquired offline and marked with RTG BG queue metadata.
- On login, helper is promoted into RTG-owned live helper state.
- Helper receives a queue grace window and immediate queue dispatch attempt.
- If still idle, helper retries queue dispatch on a short timer.
- Generic retirement is blocked while queue grace, queue state, invite state, battleground state, or active RTG demand still exists.
- Once demand vanishes and the helper is no longer queue-/BG-owned, retirement is allowed.

--------------------------------

## Known Constraints

- Requires mod-playerbots and RTG event-driven queue assistance to be enabled.
- Still depends on `AiPlayerbot.MaxRandomBots > 0` because helper existence uses the live playerbot online population path.
- Visibility of breadcrumbs depends on server log configuration, but this revision emits runtime breadcrumbs through visible log calls intended for `worldserver` output.

--------------------------------

## Future Evolution Hooks

- helper burst-queue mode for deterministic BG startup
- per-helper queue attempt counters and queue failure diagnostics
- stronger differentiation between StarterFill and LiveBalance retirement timing
- scoreboard / world-activity integration for future demand weighting

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.h`
- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- `conf/playerbots.conf.dist`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision
Fixes a mature-phase acquisition bug where finish_fill and live_refill demand could be visible in planner logs but no new helpers would log in because AddRandomBots subtracted all currently online RTG-managed bots from the unresolved demand. RTG event-driven acquisition now honors incremental unresolved need, limited only by real MaxRandomBots headroom.

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Set `AiPlayerbot.MaxRandomBots` to a positive value, such as `20`.
3. Keep `AiPlayerbot.RandomBotAccountCount` high enough to supply helpers.
4. Enable RTG event-driven queue assistance and queue ownership.
5. Restart `worldserver`.
6. Queue one real level-19 player into WSG.
7. Confirm helpers log in and immediately attempt BG queue.
8. Confirm helpers stay online during queue grace instead of idling out instantly.
9. Leave the battleground queue on the real player.
10. Confirm helper bots respond to vanished demand and retire once no queue/BG ownership remains.
11. Watch for runtime breadcrumbs in `worldserver` during control, acquire, login, queue dispatch, and retirement.

--------------------------------

## Notes For RTG Brain Ingestion

This revision closes the behavioral gap between “helper exists online” and “helper behaves like a queue worker.” The important semantic change is that RTG BG helpers are now treated as directed queue actors with immediate queue dispatch and a temporary grace shield, rather than passive randombots waiting for normal AI cadence to decide to join queue.
