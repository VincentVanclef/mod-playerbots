# Module Brain Addendum

Module Name: mod-playerbots
Module Version: RTG Queue Assistance 2.3.2
RTG Brain Compatibility Version: 2.3.0
Commit Title: Restore standalone RTG update tick and visible demand breadcrumbs
Commit Description: Fixes a regression where `RandomPlayerbotMgr::UpdateAIInternal` still returned early whenever `AiPlayerbot.RandomBotAutologin = 0`, which prevented standalone RTG queue assistance from running acquisition and queue control at all. Also adds state-change demand breadcrumbs so `worldserver` has visible RTG runtime confirmation when helper demand and target bot count change.

--------------------------------

## Module Purpose

This revision restores the intended standalone RTG battleground helper control path. It ensures the live manager tick continues to run when RTG event-driven assistance is enabled even if generic randombot autologin is disabled.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr::UpdateAIInternal` remains the live control loop for RTG helper acquisition and lifecycle.
- RTG event-driven demand still derives target helper counts from LFG and battleground need events.
- Runtime breadcrumbs are emitted from the same manager loop so control-path activation can be confirmed directly from `worldserver` output.

--------------------------------

## Runtime Control Path

real player queues battleground
→ RTG planner raises demand events
→ `UpdateAIInternal` stays alive even with `RandomBotAutologin = 0`
→ RTG computes target helper population
→ helper acquisition/login logic executes
→ queue dispatch / retirement logic follows from the live manager path

--------------------------------

## Data Structures

- Existing RTG queue ledger and metadata remain unchanged in this revision.
- Existing event-cache keys remain the source of RTG demand and helper state.

--------------------------------

## Config Interface

- `AiPlayerbot.RTG.EventDriven.Enable` must be enabled for standalone RTG control.
- `AiPlayerbot.RandomBotAutologin = 0` is now compatible with RTG standalone helper control again.
- `AiPlayerbot.MaxRandomBots` must still be greater than 0 for helpers to be allowed online.

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- RTG battleground demand planner/event cache
- existing RTG queue ownership/lifecycle support

--------------------------------

## Lifecycle Model

This revision does not change helper state definitions. It restores access to the live runtime tick so those lifecycle stages can actually execute under standalone RTG mode.

--------------------------------

## Known Constraints

- Standalone RTG control still depends on the global random-bot online cap being above zero.
- Runtime breadcrumbs depend on the server showing warning/info messages for the `playerbots` log category.

--------------------------------

## Future Evolution Hooks

- Demand-transition breadcrumbs now make it easier to validate future queue-dispatch and retire-gate changes.
- The restored standalone control path is the foundation for later single-controller RTG queue orchestration.

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Standalone RTG queue assistance no longer stops at the top of `UpdateAIInternal` when generic randombot autologin is disabled.
- `worldserver` should now receive a visible `[RTG][CONTROL]` breadcrumb once and `[RTG][DEMAND]` breadcrumbs whenever RTG demand/target state changes.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.Enabled = 1`.
2. Set `AiPlayerbot.RandomBotAutologin = 0`.
3. Set `AiPlayerbot.MaxRandomBots` to a positive value such as `20`.
4. Enable `AiPlayerbot.RTG.EventDriven.Enable = 1` and RTG debug options.
5. Restart `worldserver`.
6. Queue one real level 19 player into WSG.
7. Confirm `[RTG][CONTROL]` appears once and `[RTG][DEMAND]` appears when queue demand changes.
8. Confirm helper acquisition/login resumes.
9. Leave the queue and verify a new `[RTG][DEMAND]` line reflects the reduced or cleared BG demand.

--------------------------------

## Notes For RTG Brain Ingestion

This revision corrects a control-path regression rather than changing queue semantics. The key semantic rule is: standalone RTG helper mode must bypass the generic autologin gate without bypassing the rest of the live manager loop.
