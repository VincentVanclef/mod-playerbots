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
