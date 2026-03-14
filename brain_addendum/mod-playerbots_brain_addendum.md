# Module Brain Addendum

Module Name: mod-playerbots
Module Version: RTG Queue Assistance 2.3.7a
RTG Brain Compatibility Version: 2.3.0+
Commit Title: Fix desiredBracket scope in live-refill recycle guard
Commit Description: Hoists desired battleground bracket lookup out of the inner scope in RandomPlayerbotMgr so live-refill stale-helper recycling can compile correctly when referencing the bracket for recycle checks and breadcrumbs.

--------------------------------

## Module Purpose

Provides RTG-controlled helper acquisition, queueing, protection, refill, and retirement behavior for battleground assistance without depending on generic randombot autologin.

--------------------------------

## Architecture Overview

- RandomPlayerbotMgr runtime tick controls live helper lifecycle.
- RtgBgQueuePlanner computes battleground demand and maturity phase.
- RTG queue ledger tracks ownership/lifecycle.
- Event cache stores transient helper intent and protection markers.

--------------------------------

## Runtime Control Path

real player queues
→ planner computes starter-fill or live-refill demand
→ helper acquired and logged in
→ helper queued
→ lifecycle keeps helper protected while needed
→ stale outsider recycled or helper retired when demand ends

--------------------------------

## Data Structures

- `RTG::RtgQueueLedger`
- RTG event-cache keys such as `rtg_bg_pending`, `rtg_bg_queue_grace`, `rtg_bg_phase_*`
- planner phase values for battleground maturity

--------------------------------

## Config Interface

Uses existing RTG queue-assistance config from prior revisions, including:

- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.RTG.QueueOwnership.Debug`

--------------------------------

## Database Structures

Module currently uses no DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `RtgBgQueuePlanner`
- battleground queue/join actions
- AzerothCore battleground templates and bracket helpers

--------------------------------

## Lifecycle Model

Helpers are acquired, logged in, queued, protected while needed, and recycled or retired once demand or phase state no longer justifies them.

--------------------------------

## Known Constraints

- depends on battleground bracket lookup succeeding for recycle checks
- assumes RTG event-driven queue assistance is enabled
- assumes helper lifecycle state lives in memory

--------------------------------

## Future Evolution Hooks

- richer per-helper refill diagnostics
- tighter synchronized release when demand collapses
- planner handoff refinements for mature battleground refill

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

No intended gameplay behavior change. This is a compile-safety hotfix that preserves live-refill recycle behavior while fixing the bracket variable lifetime.

--------------------------------

## Test Plan

1. Apply this hotfix over the prior RTG queue-assistance patch set.
2. Rebuild mod-playerbots.
3. Confirm `RandomPlayerbotMgr.cpp` no longer errors on `desiredBracket` being undeclared.
4. Run the one-player WSG startup test.
5. Confirm planner and helper acquisition still behave as before.

--------------------------------

## Notes For RTG Brain Ingestion

This revision fixes a C++ scope regression introduced in the live-refill recycle path. The bug was not in planner math; it was caused by referencing `desiredBracket` outside the `if` initializer that declared it.
