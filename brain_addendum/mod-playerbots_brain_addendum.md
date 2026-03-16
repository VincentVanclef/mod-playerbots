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

---

## Addendum: RTG Queue Recovery — compile-safe event-driven return restoration

### Version note

This addendum documents the recovery applied after a bad merge damaged the closing structure of the RTG event-driven helper block inside `RandomPlayerbotMgr::AddRandomBots()`.

### Symptom

The module stopped compiling with parser-style errors beginning near the end of the RTG event-driven acquisition branch, including messages like:

- `expected expression`
- `function definition is not allowed here`
- large cascades of false downstream syntax errors

Those errors were not independent defects. They were the normal fallout of one corrupted control-flow boundary near the bottom of the RTG helper acquisition block.

### Root cause

The RTG event-driven branch is designed to short-circuit out of `AddRandomBots()` once RTG-managed acquisition has finished its own demand calculation and helper reservations.

A broken merge removed that intended early return shape and allowed the legacy alliance/horde phased login block to be parsed as if it belonged to the RTG branch. That shifted the surrounding `if / else` structure out of alignment and caused the compiler to interpret subsequent method definitions as nested inside `AddRandomBots()`.

### Resolution

The recovery restored the known-good tail structure of the function:

1. preserve RTG event-driven acquisition result logging
2. preserve missing-bot timer handling for unresolved RTG demand
3. restore the intended early return from the RTG branch
4. restore the legacy phased login branch as the fallback non-RTG path
5. restore the outer `else` that clears the missing-bot timer when enough bots already exist

### Why this mattered to queue testing

This recovery is a **structural compile repair**, not the final queue-behavior fix by itself.

Without this restoration, every subsequent queue test was invalid because the build was not structurally trustworthy.

With the RTG event-driven branch closed correctly again, future startup / dispatch / finish-fill debugging can once again be reasoned about from real runtime behavior instead of merge corruption.

### Expected result from this recovery

After this fix, the module should:

- compile past the brace/parser failure region
- preserve RTG event-driven helper acquisition semantics
- preserve legacy non-RTG fallback login behavior
- allow the next queue test to focus on real startup starvation rather than syntax damage

### Next investigation layer

If multi-BG startup still plateaus at ~13 bots after compile recovery, the next live causes to inspect remain:

- dispatch starvation after first-BG launch
- finish-fill consuming capacity before startup lanes finish
- pending helper ownership / queue residue preventing same-account reuse
- lane fairness between queue 2 / queue 3 / queue 4 under shared BG ceilings
