# mod-playerbots RTG queue addendum

## Patch focus

This patch corrects battleground acquisition phase ordering so that startup queues are fulfilled before finish-fill growth on already-running battlegrounds.

## Problem observed

When one battleground had already launched and entered `finish_fill`, the acquisition sorter still prioritized `finish_fill` buckets ahead of a second battleground still in `pop_or_invite`.

That caused the first battleground to keep asking for expansion bots while the second battleground remained below launch threshold.

## Formula correction

BG phase priority is now:

1. `pop_or_invite`
2. `starter_fill`
3. `live_refill`
4. `finish_fill`

Additionally, when any BG bucket still has startup demand (`pop_or_invite` or `starter_fill`), `finish_fill` buckets are suppressed for that acquisition pass.

## Intent

- launch waiting battlegrounds before enlarging already-running ones
- preserve live-refill behavior
- stop first-BG finish-fill growth from starving second-BG startup

---

## Addendum — same-tick dispatch refresh after queue acquisition

### Patch focus

This revision fixes a regression where RTG queue helpers were being successfully **acquired** into `currentBots`, but the same world tick still dispatched against an older pre-acquisition `availableBots` snapshot.

### Problem observed

Under multi-queue pressure:

- the first battleground in the tick could acquire and dispatch normally
- later battlegrounds or RDF helpers could also be acquired in the same tick
- but those later helpers were **not visible to ProcessBot login dispatch until the next tick**
- meanwhile the stall watchdog kept aging those helpers as pending queue-managed `add` entries
- result: logs showed large `ACQUIRE` waves, but only the first batch produced `DISPATCH][ADD]` and `LOGIN`

This matched the live symptom of:

- first BG starts
- second / third BG stays stuck in `pop_or_invite`
- `finish_fill` or `live_refill` helpers keep appearing later
- many pending helpers eventually emit `DISPATCH][STALL]`

### Root cause

`UpdateAIInternal` built these dispatch-side views **before** `AddRandomBots()` ran:

- `availableBots`
- `availableBotCount`
- `onlineBotCount`

When `AddRandomBots()` appended newly acquired RTG helpers into `currentBots`, the dispatch loop still used the stale snapshot built earlier in the tick.

A previous hotfix attempt tried to refresh dispatch state, but placed that logic inside `AddRandomBots()` using locals that only exist in `UpdateAIInternal`, which caused the compile failure around undeclared identifiers such as:

- `availableBots`
- `availableBotCount`
- `onlineBotCount`
- `pendingQueuedLogins`
- `intervalCap`

### Resolution

This patch does two things:

1. removes the misplaced post-acquire dispatch-budget block from `AddRandomBots()`
2. refreshes dispatch state in the correct scope, inside `UpdateAIInternal`, immediately after `AddRandomBots()` can mutate `currentBots`

Refreshed values now include:

- `availableBots = currentBots`
- `availableBotCount = availableBots.size()`
- `onlineBotCount = playerBots.size()`

A new trace was also added:

- `[RTG][DISPATCH][REFRESH] availableBotCount=... onlineBotCount=... pendingDispatchable=...`

### Intent

- make newly acquired BG/LFG helpers dispatchable in the **same tick** they are reserved
- stop second/third queue waves from being invisible until a later update
- reduce false `DISPATCH][STALL]` cleanup for healthy helpers
- preserve existing BG phase priority logic while restoring immediate dispatch correctness

### Expected behavioral change

When three battleground queues arrive in rapid succession, logs should now show:

- `ACQUIRE` for queue 4, queue 3, and queue 2 in the same tick window
- one `DISPATCH][REFRESH]` line showing the refreshed pending helper count
- `DISPATCH][ADD]` and `LOGIN` continuing beyond the first BG batch instead of stopping at ~13 bots

### Campaign status

This is a **dispatch visibility / snapshot freshness** correction, not a queue formula rewrite.

If startup still stalls after this patch, the next likely layer is no longer stale dispatch state — it will be one of:

- offline account reuse collisions across lanes
- queue ownership residue not clearing fast enough
- battleground queue metadata not being revalidated after launch transitions
