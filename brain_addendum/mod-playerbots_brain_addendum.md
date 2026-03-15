# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8aa
RTG Brain Compatibility Version: 5.4.0
Commit Title: Repair RTG battleground planner handoff and preserve RDF materialization
Commit Description: Rewires RTG battleground acquisition to consume planner-owned per-team demand directly, preventing finish_fill/live_refill starvation and keeping RDF helper materialization isolated from battleground helper assignment.

--------------------------------

## Module Purpose

This revision focuses on the two unstable surfaces still blocking playable queue assistance:

- battleground refill / finish-fill helper acquisition
- RDF helper materialization while battleground demand is also active

The revision preserves the event-driven RTG model and keeps the already-improved queue collapse / retirement behavior intact.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live orchestration layer for helper acquisition, login, immediate queue dispatch, retry, and retirement.
- `RtgBgQueuePlanner` remains the authoritative battleground planner that computes real per-team helper shortage using live queue + active battleground state.
- RTG add metadata (`rtg_bg:*`, `rtg_lfg:*`) still defines helper purpose at login time.
- RDF helpers and BG helpers continue to share the same temporary event-driven bot pool, but acquisition now respects service-specific demand more cleanly.

--------------------------------

## Runtime Control Path

real player queue demand appears
→ planner writes authoritative RTG shortage events
→ acquisition reads per-service demand
→ RDF helpers log in with role-specific metadata
→ BG helpers log in with queue/team-specific metadata
→ helper dispatch attempts occur immediately after login
→ live demand re-checks continue to materialize additional helpers for refill / mature ramp
→ orphan queue collapse and safe retirement still remove stale helpers when demand truly dies

--------------------------------

## Data Structures

- `RtgLfgBucket`
- `RtgBgBucket`
- `RtgHelperLedgerEntry`
- RTG battleground planner keys now explicitly consumed by acquisition:
  - `rtg_bg_team_need:<queue>:<bracket>:<team>`
  - `rtg_bg_phase:<queue>:<bracket>`
  - `rtg_bg_real_demand:<queue>:<bracket>`

--------------------------------

## Config Interface

This revision uses existing config only:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Debug`
- `AiPlayerbot.RTG.QueueOwnership.RetireRetrySeconds`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAccountCount`
- `AiPlayerbot.RandomBotAutologin`

No new config keys are introduced in this pass.

--------------------------------

## Database Structures

No new DB schema changes.

Battleground sizing continues to come from `world.battleground_template` through planner cache logic.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `RtgQueueLifecycle`
- `RtgQueueLedger`
- battleground queue APIs
- LFG queue APIs

--------------------------------

## Lifecycle Model

### RDF
- acquire exact role helper
- log in helper with `rtg_lfg:*`
- dispatch to RDF/LFG queue without being reclassified as BG demand
- stale RDF helpers are still retired if they never truly materialize into group/dungeon state

### Battlegrounds
- planner computes team-specific deficit from real queue + active match state
- acquisition now reads planner per-team shortage directly
- additional helpers can be logged during `live_refill` and `finish_fill`
- retirement only happens after queue collapse / lifecycle safety says helper is no longer needed

--------------------------------

## Known Constraints

- Real-player disconnects can still look like demand disappearance to the planner if the player fully vanishes from queue state. This revision does not yet add a long reconnect cushion window.
- RDF still depends on normal LFG acceptance/materialization rules in core + playerbot behavior after helper login.

--------------------------------

## Future Evolution Hooks

- real disconnect cushion / reclaim window before BG collapse
- per-owner RDF rematerialization retry windows
- stricter BG-vs-RDF pool partitioning if mixed demand becomes extremely bursty
- planner-aware priority weights for startup vs refill vs mature finish-fill

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Battleground helper acquisition no longer derives refill need from queue totals alone.
- Acquisition now consumes planner-authored per-team battleground shortage events, which keeps `live_refill` and `finish_fill` spawning alive after startup.
- Battleground acquisition ordering now prefers earlier planner phases before later mature fill pressure when multiple BG buckets compete.
- RDF helper materialization remains separate from BG helper metadata, so mixed RDF+BG demand is less likely to be swallowed by BG-only add paths.
- Two brace/flow correctness issues in RTG helper event setup/logging were cleaned up to reduce accidental state bleed.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Enable RTG event-driven queue assistance and queue ownership.
3. Queue one real level-19 player for WSG.
4. Confirm startup logs show `pop_or_invite` demand and helper login for both teams.
5. Let the battleground advance into `live_refill` and `finish_fill`.
6. Confirm new helper logins continue after the initial 7v7 start when planner demand rises to 9v9 / 11v11 / 13v13 / 15v15.
7. Start a simultaneous RDF queue from another real player.
8. Confirm RDF helpers log with `[RTG][LFG][ACQUIRE]` / `[RTG][LFG][LOGIN]` and do not get reassigned into battleground helper login metadata.
9. Leave queue demand and verify safe collapse / retirement still removes orphan helpers cleanly.
10. Watch specifically for these breadcrumbs:
   - `[RTG][BG][PHASE]`
   - `[RTG][BG][DEMAND]`
   - `[RTG][ACQUIRE][HEADROOM]`
   - `[RTG][ACQUIRE]`
   - `[RTG][LFG][ACQUIRE]`
   - `[RTG][QUEUE][DISPATCH]`
   - `[RTG][BG][CLEAR]`

--------------------------------

## Notes For RTG Brain Ingestion

The important semantic correction is that RTG battleground acquisition must consume planner truth, not infer demand from raw queue counts after the fact. Startup can be approximated from queue totals, but mature RTG battleground behavior requires planner-owned per-team shortage to drive additional helper login. This revision makes that handoff explicit.
