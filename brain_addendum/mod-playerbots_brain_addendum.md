# mod-playerbots addendum — same-tick dispatch refresh hotfix

## Date
2026-03-16

## Scope
Hotfix for the RTG queue-helper startup regression and the follow-up compile break.

## Problem chain
A prior fix correctly identified that RTG queue-helper acquisition could append newly reserved helpers into `currentBots` during `AddRandomBots()`, but the dispatch layer in `UpdateAIInternal()` was still using the older `availableBots` snapshot from earlier in the same tick.

That caused this pattern:
- `ACQUIRE` succeeded for later BG/LFG queues
- only the first visible wave got `DISPATCH][ADD]` and `LOGIN`
- later helpers stayed invisible until a future tick
- under pressure they aged into `DISPATCH][STALL]`

A first hotfix attempt then moved refresh logic into `AddRandomBots()`, but it referenced `UpdateAIInternal()` locals such as:
- `availableBots`
- `availableBotCount`
- `onlineBotCount`
- `pendingDispatchable`

That placement corrupted control flow in `RandomPlayerbotMgr.cpp` and produced compile failures around the `else` chain after `AddRandomBots()`.

## Resolution
This hotfix does two things:

1. Restores `AddRandomBots()` structure by removing the misplaced refresh block.
2. Applies the same-tick refresh only in `UpdateAIInternal()`, immediately after `AddRandomBots()` can mutate `currentBots`.

The safe refresh now does:
- `availableBots = currentBots;`
- `availableBotCount = availableBots.size();`
- `onlineBotCount = playerBots.size();`

It also emits a debug line when RTG queue debug is enabled:
- `[RTG][DISPATCH][REFRESH] availableBotCount=... onlineBotCount=... pendingDispatchable=...`

## Expected effect
This should allow newly acquired RTG queue helpers to become dispatch-visible in the same world tick instead of waiting for the next cycle.

Expected next-test behavior:
- queue 4 should no longer monopolize first visibility
- queue 3 and queue 2 startup helpers should also reach `DISPATCH][ADD]` and `LOGIN`
- multi-BG startup should move beyond the first ~13 helper logins

## Remaining possible next bottlenecks if startup still caps low
If startup still stalls after this compile-safe hotfix, the next likely layers are:
- helper/account collision across simultaneous queue ownership
- dispatch budget starvation caused by live-refill waves consuming same-tick headroom
- stale queue ownership state preventing later queue lanes from reusing already-acquired helpers correctly
