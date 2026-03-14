# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.7f
RTG Brain Compatibility Version: 5.4.0
Commit Title: Immediate BG queue burst dispatch for RTG helpers
Commit Description: Restores near-simultaneous battleground helper queueing by dispatching battleground join immediately on helper login and retrying pending helpers on a short cadence while demand still exists.

--------------------------------

## Module Purpose

This revision improves RTG battleground startup responsiveness so helpers do not drift into slow staggered queue joins after logging in.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the live RTG helper acquisition and retention controller.
- Battleground planner still computes demand and team targets.
- This revision adds a direct post-login queue-dispatch path plus a short pending retry loop for online helpers.

--------------------------------

## Runtime Control Path

real player queues
→ planner computes BG demand
→ RTG acquires helpers
→ helper logs in
→ `RandomPlayerbotMgr::OnBotLoginInternal` dispatches `bg join` immediately
→ pending helpers retry dispatch every short interval until queued or demand disappears
→ normal lifecycle / retirement rules apply afterward

--------------------------------

## Data Structures

- existing RTG event cache entries
- `rtg_bg_pending`
- `rtg_bg_dispatch_retry`
- existing BG planner demand keys

--------------------------------

## Config Interface

No new config entries in this revision.

Relevant existing config:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.Debug`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAutologin`

--------------------------------

## Database Structures

Uses existing battleground planner inputs only. Module currently uses no new DB persistence.

--------------------------------

## Integration Points

- `RandomPlayerbotMgr`
- `PlayerbotAI::DoSpecificAction`
- `BattleGroundJoinAction`
- RTG battleground planner demand events

--------------------------------

## Lifecycle Model

Helpers stay RTG-owned through pending queue work, but now receive an immediate queue action on login and a short retry pulse while pending and still needed.

--------------------------------

## Known Constraints

- Requires the RTG standalone control tick to already be working.
- Assumes `bg join` action is valid immediately after bot login on this branch.
- Retry cadence is event-driven and intentionally short to favor synchronized startup pops.

--------------------------------

## Future Evolution Hooks

- Burst dispatch grouping by queue/bracket/team
- Explicit invite detection metrics
- Smarter retry backoff for mature live-refill phases

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- BG helpers now attempt queue join immediately on login.
- Pending helpers retry queue join on a short cadence instead of idling until slower generic AI timing catches up.
- Queue-dispatch breadcrumbs now show login-success and pending-retry reasons.

--------------------------------

## Test Plan

1. Start worldserver with standalone RTG queue assistance enabled.
2. Queue solo for WSG and confirm multiple helpers log in and queue nearly together.
3. Watch for `[RTG][QUEUE][DISPATCH]` lines with `reason=login_success` across the acquired helper set.
4. Confirm remaining pending helpers emit `reason=pending_retry` if they are not queued immediately.
5. Repeat with EOTS and verify startup is no longer slow/staggered.

--------------------------------

## Notes For RTG Brain Ingestion

This revision is intentionally narrow: it restores synchronized queue burst behavior without redesigning planner demand. It should be evaluated on top of the currently working visible-logging standalone RTG baseline.
