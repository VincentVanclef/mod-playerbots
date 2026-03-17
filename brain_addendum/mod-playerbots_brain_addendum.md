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


## Patch focus

This patch refines adaptive RTG queue helper levels and restores queue-bot autogear after event-driven level changes.

## Problems observed

- Mixed-level battleground queues were still clustering too tightly around the midpoint.
- A level 10 and level 19 queue pair still produced mostly level 14–16 helpers, which felt unfair to both ends of the bracket.
- Queue helpers stopped reliably regenerating gear after level sync on login.

## Formula correction

Adaptive helper levels now use a weighted spread built from the real queued population for the same battleground bracket or RDF owner group:

- center band: average level plus a random offset from -3 to +3
- lower tail: random values from the real minimum up to just below the average
- upper tail: random values from just above the average up to the real maximum

This keeps most helpers near the midpoint while still populating the low and high ends of the real-player level spread.

For example:
- real players level 10 and 19
- midpoint ≈ 15
- helper distribution can now include 10–14, 12–18, and 16–19 instead of clustering almost entirely at 14–16

## Gear restoration

When an event-driven helper is level-synced on login, the bot now rebuilds equipment and refreshes combat preparation so queue helpers do not remain in stale low-level gear.

## Intent

- preserve the working queue system
- make mixed-level brackets feel fairer to fresh and twink players alike
- keep helper gearing consistent with synchronized helper level


## Patch focus

This patch repairs battleground phase semantics and queue-helper login preparation after the revert storm.

## Problems observed

- Battleground phase values drifted out of sync between the planner and the acquisition sorter.
- `pop_or_invite` buckets were being treated like later phases during acquisition ordering.
- `finish_fill` suppression checks were looking for phase `3` even though `finish_fill` is phase `4` in the overlay planner.
- Queue helpers could log in at the correct synchronized level but still rely on stale pre-existing preparation unless their level actually changed.

## Repair details

- Re-aligned BG phase priority mapping in `RandomPlayerbotMgr.cpp` with the planner contract used by `RtgBgQueuePlanner.cpp`:
  - `2 = pop_or_invite`
  - `1 = starter_fill`
  - `3 = live_refill`
  - `4 = finish_fill`
- Re-aligned startup detection so `pop_or_invite` really suppresses `finish_fill` acquisition during startup pressure.
- Re-aligned all `finish_fill` suppression checks to phase `4`.
- Centralized queue-helper login preparation into one helper path that:
  - applies synchronized helper level
  - rebuilds equipment with second-chance replacement
  - refreshes spells / talents / consumables
  - clears stale queue debuffs
  - resets strategies so the helper starts its queue job in a clean state

## Intent

- restore battleground startup-before-growth discipline after the regressions
- stop `finish_fill` from stealing acquisition cycles while launch buckets still need bodies
- make synchronized queue helpers enter the world battle-ready immediately
- keep the current BG/LFG ownership pipeline intact while stabilizing future arena-lane expansion groundwork


## 2026-03-17 — RTG queue assistance regression intake: hidden account-pool starvation

### Symptom
Simultaneous low-bracket WSG + AB + EOTS demand produced valid `[RTG][ACQUIRE][REQUEST]` logs, but only the first wave of helpers actually logged in. Later waves stalled and the runtime emitted:

- `Can't log-in all the requested bots. Try increasing RandomBotAccountCount in your conf file.`
- repeated `[RTG][DISPATCH][STALL]` entries for queue-managed add data

### Root cause
This was not primarily a battleground planner failure. The queue-assistance lane was being starved by **account-type assignment math**.

In RTG event-driven mode, helper acquisition already draws from `rndBotTypeAccounts`. However `AssignAccountTypes()` was still sizing the RNDbot type-1 pool from the live helper ceiling (`RTG.EventDriven.MaxBots`) divided by available characters per account. With `MaxBots=300`, that typically assigns only about 30 type-1 accounts even if `RandomBotAccountCount=1200` already exists in the database.

That creates a false starvation pattern:
- enough total accounts exist
- planner computes demand correctly
- acquire requests fire correctly
- but the usable type-1 account pool is only ~30 accounts
- multi-lane demand exhausts those accounts and starts reporting missing accounts that are not actually missing

### Fix direction
For RTG event-driven mode, treat the configured randombot account pool as available queue-helper capacity. Assign all configured non-AddClass accounts into the RNDbot type-1 pool instead of sizing type-1 strictly from the current gameplay ceiling.

### Diagnostics added
Add explicit RTG control logging for:
- configured randombot account count
- desired RNDbot account count
- actually assigned RNDbot account count
- AddClass reserve count
- total available randombot accounts discovered in DB

Also improve the event-driven shortage error to include assigned RNDbot account count and busy-account count so future sessions can distinguish:
- true DB/account shortage
- false starvation due to assignment math
- temporary lane exhaustion due to busy accounts

### Arena scaffolding note
The queue system still needs a dedicated arena lane rather than treating arena demand as battleground demand. The safe next step is:
1. keep BG/RDF stable
2. preserve lane separation doctrine
3. add arena demand/planner/dispatch scaffolding as a third explicit service lane
4. only then enable real arena helper materialization
