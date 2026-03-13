# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.2.2
RTG Brain Compatibility Version: 2.3.0
Commit Title: Protect RTG queue helpers through live logout gates
Commit Description: Wires RTG ownership into live logout and retirement decisions, promotes successful helper logins into protected owned-online state, and adds visible worldserver breadcrumbs for helper acquisition, login, protection, retirement, and login-failure cleanup.

--------------------------------

## Module Purpose

This module supplies autonomous and RTG-managed playerbots inside the AzerothCore realm. In this revision, the RTG battleground queue-assistance path is pushed further into the real live runtime so helper bots can be acquired on demand, brought online, protected from generic cleanup while still RTG-owned, and retired only after demand is gone and lifecycle release is safe.

--------------------------------

## Architecture Overview

- **Manager classes**
  - `RandomPlayerbotMgr` remains the real live controller for helper acquisition, login scheduling, queue/lifecycle audits, and safe logout.
  - `RtgQueueLedger` holds the in-memory semantic ownership ledger for RTG helper bots.
  - `RtgBgQueuePlanner` computes battleground demand overlays from real queue state.
- **Runtime loops**
  - `RandomPlayerbotMgr::UpdateAIInternal` is the main live tick.
  - `CheckBgQueue` computes battleground demand and refreshes helper assignment intent.
  - `RTG_RunQueueOwnershipAudit` validates transitional ownership and safe-release timing.
- **Data structures**
  - `RtgHelperLedgerEntry` stores owner type, purpose, queue target, protection window, transition timestamps, and retirement intent.
- **Ownership models**
  - `QueueDemand` means the helper is still owned by active RTG demand / queue work.
  - `Battleground` means the helper is owned by a live battleground lifecycle state.
- **Lifecycle control**
  - Helpers transition through `LoggingIn`, `WorldIdle`, `Queued`, `Invited`, `InBattleground`, `Releasing`, and `Retired`.
  - Successful login now explicitly promotes the helper into a protected owned-online state instead of leaving it in an unprotected transitional window.
- **Event hooks**
  - `OnBotLoginInternal` stamps live ownership and post-login protection.
  - `OnPlayerLoginError` clears failed pending helper state and logs it.

--------------------------------

## Runtime Control Path

real player queues for battleground
→ `CheckBgQueue` detects real demand and sets RTG demand events
→ `UpdateAIInternal` remains alive for RTG control even with generic autologin disabled
→ `AddRandomBots` selects offline helper candidates
→ helper login request stamps `add` event + RTG pending flags + pending ownership ledger entry
→ `ProcessBot` performs the actual live login path
→ `OnBotLoginInternal` immediately upgrades the helper from pending login into a protected owned-online RTG helper
→ battleground join logic queues the helper
→ `SyncBgHelperState` upgrades helper state through queued / invited / battleground ownership
→ generic shrink/logout gates now skip RTG-owned active helpers
→ retirement is allowed only after demand is gone, queue/invite/BG ownership is gone, and lifecycle review says release is safe

--------------------------------

## Data Structures

- `RtgHelperLedgerEntry`
  - In-memory semantic record for one RTG-managed helper bot.
  - Stores `purpose`, `state`, `ownerType`, battleground target key, timestamps, protection window, and retirement flags.
- `RtgHelperState`
  - Explicit helper states: `Reserved`, `LoggingIn`, `WorldIdle`, `Queued`, `Invited`, `InBattleground`, `Releasing`, `Retired`.
- `RtgHelperOwnerType`
  - Explicit ownership split: `QueueDemand` or `Battleground`.
- `RtgLifecycleResult`
  - Safe-retirement decision object used to delay or allow helper release.

--------------------------------

## Config Interface

This revision uses the existing RTG config surface:

- `AiPlayerbot.RTG.EventDriven.Enable`
  - Enables the standalone RTG queue-helper control path.
  - In this revision it is documented as keeping the manager tick alive for RTG helper control even when `AiPlayerbot.RandomBotAutologin = 0`.
- `AiPlayerbot.RTG.SmartQueue.Enable`
  - Enables smarter battleground demand prioritization.
- `AiPlayerbot.RTG.QueueGraceSeconds`
  - Grace timing for queue transitions and helper retirement churn control.
- `AiPlayerbot.RTG.EventDriven.MaxBots`
  - Caps RTG-managed helper count.
- `AiPlayerbot.RTG.EventDriven.KeepWorldBots`
  - Controls whether helpers can remain online as world bots after RTG demand ends.
- `AiPlayerbot.RTG.EventDriven.Debug`
  - Enables focused event-driven RTG debug logging.
- `AiPlayerbot.RTG.QueueOwnership.Enable`
  - Enables the in-memory helper ownership ledger.
- `AiPlayerbot.RTG.QueueOwnership.ProtectInBattleground`
  - Protects helpers during queue/invite/battleground lifecycle ownership.
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
  - Retry delay for helpers whose retirement is delayed.
- `AiPlayerbot.RTG.QueueOwnership.MaxTransitionSeconds`
  - Watchdog threshold for helpers stuck too long in transitional states.
- `AiPlayerbot.RTG.QueueOwnership.Debug`
  - Enables focused ownership debug logging.
- `AiPlayerbot.MaxRandomBots`
  - Still acts as the global live randombot existence cap. In this revision the config documentation explicitly warns that it must be greater than 0 for RTG helpers to exist online.

--------------------------------

## Database Structures

Module currently uses no DB persistence for RTG queue ownership.

RTG helper ownership, purpose, and lifecycle state remain in memory only in this revision.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgQueueLifecycle`
- `RtgQueueLedger`
- `CheckBgQueue`
- `BattleGroundJoinAction`
- AzerothCore battleground queue, invite, and battleground map lifecycle

--------------------------------

## Lifecycle Model

- Offline helper candidates are selected by RTG demand.
- `RegisterPendingHelperLogin` records immediate pending ownership while the helper is still offline / logging in.
- On successful login, `RecordHelperReservation` now promotes the helper into `WorldIdle` with `QueueDemand` ownership and a short protection window.
- Generic shrink / logout logic now treats RTG-owned active helpers as protected.
- `EvaluateRetire` now delays retirement when RTG demand or pending helper work is still active.
- Retirement only proceeds after the helper is no longer queue-owned, no longer queued/invited/in-battleground, and the lifecycle gate allows it.
- Failed login removes pending RTG state immediately.

--------------------------------

## Known Constraints

- Requires `AiPlayerbot.Enabled = 1`.
- Requires `AiPlayerbot.MaxRandomBots > 0` so helper bots are allowed to exist online.
- Ownership remains in memory only and is cleared on restart.
- The system still relies on existing battleground join logic to finish queue entry after helper login.
- This revision improves live protection and observability but does not introduce DB-backed persistence.

--------------------------------

## Future Evolution Hooks

- Separate admin counters for helpers in `LoggingIn`, `WorldIdle`, `Queued`, `Invited`, and `InBattleground`.
- Richer candidate diagnostics for “eligible vs skipped” helper selection.
- Optional queue-purpose balancing between starter fill and live-balance helpers.
- Better GM-facing summaries of helper protection and delayed retire reasons.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- `conf/playerbots.conf.dist`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Successful RTG helper login now immediately creates a protected owned-online helper state instead of leaving the bot exposed to generic cleanup.
- Generic shrink/logout selection now skips RTG-owned active helpers.
- Lifecycle retirement evaluation now delays logout when RTG queue demand or pending helper work is still active.
- Worldserver breadcrumbs are now emitted for helper acquisition requests, helper login success, protection-blocked retirement, allowed retirement, and failed helper login cleanup.
- Config documentation now explicitly explains the RTG standalone tick behavior and the need for `AiPlayerbot.MaxRandomBots > 0`.

--------------------------------

## Test Plan

1. Set:
   - `AiPlayerbot.Enabled = 1`
   - `AiPlayerbot.RandomBotAutologin = 0`
   - `AiPlayerbot.MaxRandomBots = 20` or higher
   - `AiPlayerbot.RandomBotJoinBG = 1`
   - `AiPlayerbot.RTG.EventDriven.Enable = 1`
   - `AiPlayerbot.RTG.QueueOwnership.Enable = 1`
   - `AiPlayerbot.RTG.EventDriven.Debug = 1`
   - `AiPlayerbot.RTG.QueueOwnership.Debug = 1`
2. Restart worldserver.
3. Confirm the worldserver log shows:
   - `[RTG][CONTROL] Standalone queue-helper control active...`
4. Queue one real level-19 player into Warsong Gulch.
5. Confirm the log shows helper acquisition requests:
   - `[RTG][ACQUIRE][REQUEST] ...`
6. Confirm successful helper logins now produce:
   - `[RTG][LOGIN] helper=... online ...`
7. Verify the helpers remain online long enough to queue instead of instantly logging out.
8. If a generic retire path attempts to remove an active helper early, confirm the log shows:
   - `[RTG][PROTECT] Blocking logout for helper bot ...`
9. After the battleground ends and need is gone, confirm the log eventually shows:
   - `[RTG][RETIRE] Allowing helper bot ... logout ...`
10. Optionally force a login failure and confirm:
   - `[RTG][LOGIN][FAIL] helper=... login failed; cleared RTG pending state`

--------------------------------

## Notes For RTG Brain Ingestion

This revision is the first meaningful pass that pushes RTG ownership directly into the real live logout gates rather than only into acquisition and battleground-state scaffolding. The semantic meaning for the RTG brain is that helper lifetime authority is shifting away from legacy randombot churn and toward explicit RTG ownership plus lifecycle review. Successful helper login is now treated as a protected semantic milestone in the runtime model.
