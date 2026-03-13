# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.2.1
RTG Brain Compatibility Version: 2.3.0
Commit Title: RTG queue helpers run standalone without generic autologin
Commit Description: Wires the live RandomPlayerbotMgr control tick so RTG event-driven queue assistance remains active even when generic RandomBotAutologin is disabled, and hardens failed-helper cleanup so stale pending ownership state is removed immediately.

--------------------------------

## Module Purpose

This module supplies autonomous and RTG-managed playerbots inside the AzerothCore realm. In this revision, the RTG battleground and LFG queue-assistance path is treated as a live runtime controller rather than a passive overlay. Its purpose is to detect real demand, acquire offline helper bots, bring them online, attach RTG ownership metadata, queue them into the proper content, keep them protected while battleground lifecycle still owns them, and retire them safely afterward.

--------------------------------

## Architecture Overview

- **Manager classes**
  - `RandomPlayerbotMgr` is the real runtime controller for helper acquisition, login scheduling, queue state audits, and helper retirement.
  - `RtgBgQueuePlanner` computes battleground demand overlays from real queue state.
  - `RtgQueueLedger` holds the in-memory ownership ledger for RTG helper bots.
- **Runtime loops**
  - `RandomPlayerbotMgr::UpdateAIInternal` is the primary tick.
  - `CheckBgQueue` and `CheckLfgQueue` compute demand and maintain queue/lifecycle state.
  - `ProcessBot` is the login/update path for bots with a live `add` event.
- **Data structures**
  - `RtgHelperLedgerEntry` stores owner type, purpose, state, protection window, and retirement intent.
- **Ownership models**
  - Demand ownership is represented as `QueueDemand`.
  - Live battleground ownership is represented as `Battleground`.
- **Lifecycle control**
  - RTG helper bots transition through reserved/login/queued/invited/in-battleground/releasing/retired states.
  - Safe retirement is gated through `EvaluateRetire` and `RTG_RequestSafeBotLogout`.
- **Event hooks**
  - Login success and login failure are integrated through `OnBotLoginInternal` and `OnPlayerLoginError`.

--------------------------------

## Runtime Control Path

real player queues
→ `CheckBgQueue` / `CheckLfgQueue` detect demand
→ RTG demand events update `rtg_*` global keys
→ `UpdateAIInternal` stays alive in standalone RTG mode even if `AiPlayerbot.RandomBotAutologin = 0`
→ `AddRandomBots` selects offline helper candidates and stamps `add` + RTG pending events
→ `RegisterPendingHelperLogin` creates immediate in-memory helper ownership records
→ `ProcessBot` executes the real login path for those candidates
→ `OnBotLoginInternal` applies RTG reservation metadata and pending queue ownership
→ join actions queue helpers into the correct battleground/LFG path
→ `SyncBgHelperState` upgrades ownership during queue / invite / battleground transitions
→ lifecycle protection delays retirement while helpers are still battleground-owned
→ `RTG_RequestSafeBotLogout` retires helpers when safe

--------------------------------

## Data Structures

- `RtgHelperLedgerEntry`
  - In-memory semantic record for one RTG-managed helper bot.
  - Stores `purpose`, `state`, `ownerType`, target queue data, protection window, and retire flags.
- `RtgHelperState`
  - Explicit helper states: `Reserved`, `LoggingIn`, `WorldIdle`, `Queued`, `Invited`, `InBattleground`, `Releasing`, `Retired`.
- `RtgHelperOwnerType`
  - Explicit ownership split: `QueueDemand` or `Battleground`.
- `BattlegroundInfo`
  - Split queue-vs-active battleground counters used by RTG demand planning.

--------------------------------

## Config Interface

This revision uses the existing RTG config surface:

- `AiPlayerbot.RTG.EventDriven.Enable`
  - Enables the standalone RTG queue-assistance control path.
  - In this revision, it also keeps the manager tick alive even when `AiPlayerbot.RandomBotAutologin` is disabled.
- `AiPlayerbot.RTG.SmartQueue.Enable`
  - Enables smarter battleground demand planning.
- `AiPlayerbot.RTG.QueueGraceSeconds`
  - Grace window for queue transitions and helper retirement timing.
- `AiPlayerbot.RTG.EventDriven.MaxBots`
  - Caps the number of RTG-managed helpers.
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots`
  - Controls whether helpers can remain as world bots after RTG need ends.
- `AiPlayerbot.RTG.EventDriven.Debug`
  - Enables RTG queue-assistance debug breadcrumbs.
- `AiPlayerbot.RTG.QueueOwnership.Enable`
  - Enables the in-memory ownership ledger.
- `AiPlayerbot.RTG.QueueOwnership.ProtectInBattleground`
  - Applies battleground transition protection.
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
  - Retry delay for safe retirement.
- `AiPlayerbot.RTG.QueueOwnership.MaxTransitionSeconds`
  - Watchdog window for prolonged transitional states.
- `AiPlayerbot.RTG.QueueOwnership.Debug`
  - Enables ownership-layer debug breadcrumbs.

--------------------------------

## Database Structures

Module currently uses no DB persistence for RTG queue ownership.

RTG helper ownership, purpose, and lifecycle state remain in memory only in this revision.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `ProcessBot` login/update path
- `OnBotLoginInternal`
- `OnPlayerLoginError`
- `CheckBgQueue`
- `CheckLfgQueue`
- `BattleGroundJoinAction`
- AzerothCore battleground queue / invite / battleground map lifecycle

--------------------------------

## Lifecycle Model

- Helper candidates are selected while offline.
- A live `add` event reserves the helper for login.
- `RegisterPendingHelperLogin` immediately creates a queue-demand ownership record.
- On successful login, RTG battleground/LFG pending markers are reasserted and the ledger is refreshed.
- While the helper is queued, invited, or inside a battleground, ownership protection can delay retirement.
- When the helper is no longer needed and ownership clears, centralized safe logout retires it.
- If login fails before world entry, this revision clears the `add` event, RTG pending flags, and any ownership-ledger record immediately.

--------------------------------

## Known Constraints

- Requires the playerbots runtime to be enabled.
- Assumes existing RTG BG/LFG queue metadata and join actions remain wired.
- Assumes RTG battleground demand is still driven by queue observation and helper assignment metadata.
- Ownership remains in memory only; a world restart clears RTG helper semantic state.
- Standalone RTG mode still depends on the standard playerbot manager tick being active through `AiPlayerbot.Enabled`.

--------------------------------

## Future Evolution Hooks

- Stronger transition watchdog recovery for helpers stuck in `LoggingIn` / `Queued` / `Invited`.
- Smarter offline candidate scoring per battleground role/class/spec.
- Direct queue-purpose prioritization between starter fill and live-balance helpers.
- Additional observability for helper lifecycle counts in GM/admin diagnostics.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `conf/playerbots.conf.dist`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- RTG event-driven queue assistance now keeps `RandomPlayerbotMgr::UpdateAIInternal` alive even when generic `AiPlayerbot.RandomBotAutologin` is disabled.
- This makes the RTG system capable of driving helper acquisition and login through the real live manager tick instead of depending on legacy generic randombot autologin.
- Failed helper logins now clean up RTG pending queue state and ownership-ledger state immediately, reducing stale reservation drift.
- Config documentation now explicitly states that RTG event-driven mode can operate as the standalone queue-helper path.

--------------------------------

## Test Plan

1. Set:
   - `AiPlayerbot.Enabled = 1`
   - `AiPlayerbot.RandomBotAutologin = 0`
   - `AiPlayerbot.RandomBotJoinBG = 1`
   - `AiPlayerbot.RTG.EventDriven.Enable = 1`
   - `AiPlayerbot.RTG.QueueOwnership.Enable = 1`
   - `AiPlayerbot.RTG.EventDriven.Debug = 1`
   - `AiPlayerbot.RTG.QueueOwnership.Debug = 1`
2. Restart worldserver.
3. Confirm log line appears once:
   - `Standalone queue-helper control active...`
4. Log in one real level-19 player and queue Warsong Gulch.
5. Wait for `CheckBgQueue` to detect real demand.
6. Verify logs show RTG acquisition such as `[RTG][BG][ACQUIRE] ...`.
7. Verify helper bots actually log in with generic autologin still disabled.
8. Verify helpers queue for the assigned battleground rather than roaming as general world bots.
9. When the battleground starts, verify ownership debug logs show queue/invite/battleground protection rather than immediate retirement.
10. Finish or leave the battleground and confirm helpers retire only after battleground lifecycle ownership clears.
11. Optionally force a helper login failure and confirm the login-error cleanup removes pending RTG markers instead of leaving stale reservations behind.

--------------------------------

## Notes For RTG Brain Ingestion

This revision is important because it changes the control authority of RTG queue assistance. The event-driven RTG path is no longer conceptually secondary to generic randombot autologin. The real manager tick now remains live in standalone RTG mode, which makes the existing RTG acquisition, pending ownership, queue join, protection, and retirement layers actually drive runtime behavior on branches where generic autologin is intentionally disabled.
