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


## Journal entry — 2026-03-20 — Phase 3 compile recovery + RDF role-demand consolidation

### Context
A later Phase 3 attempt had started to split RDF role-planning into new helper translation units (`RtgRdfRoleResolver.cpp`, `RtgRdfQueuePlanner.cpp`) and mixed unqualified `PLAYER_ROLE_*` references with helper code that was not yet fully wired into the current module surface. That produced a compile break and widened the patch surface more than needed for this stage.

### Recovery action
This pass deliberately returned Phase 3 role-demand signaling to the already-working RTG queue spine inside `RandomPlayerbotMgr.cpp` instead of continuing the split-file branch.

### What changed
- owner-local RDF demand now also emits per-role queue events directly from the existing planner loop:
  - `rtg_lfg_need_tank`
  - `rtg_lfg_need_heal`
  - `rtg_lfg_need_dps`
- each event reuses `RTG::MakeLfgAddData(...)` and stores the exact requested role with `lfg::PLAYER_ROLE_*` values
- zero-demand roles are explicitly cleared by writing zero TTL/value, so stale role buckets decay cleanly instead of lingering from prior scans

### Why this route was chosen
This follows the RTG brain guidance to prefer narrow, local, compile-safe changes over broader architectural expansion when the current phase is still stabilizing. The module already had the necessary queue-planning context in `RandomPlayerbotMgr.cpp`; extending that path was lower-risk than introducing additional translation units while arena/RDF queue stabilization is still in flight.

### Recorded failure pattern
- unqualified `PLAYER_ROLE_*` usage caused compile failures outside the `lfg::` namespace
- speculative helper files created surface-area drift from the current module baseline
- role-planner logic was becoming duplicated between the main manager and experimental helper files

### Durable lesson
For RTG queue work, compile recovery should first collapse logic back into the proven manager path, then only extract helpers after behavior is stable and naming/include ownership is fully normalized.


## 2026-03-20 — Phase 3 compile recovery note: RDF role constants visibility
- Compile failure surfaced in `src/Bot/RtgRdfQueuePlanner.cpp` where `lfg::PLAYER_ROLE_TANK`, `lfg::PLAYER_ROLE_HEALER`, and `lfg::PLAYER_ROLE_DAMAGE` were not visible in that translation unit.
- Root cause: the Phase 3 RDF demand-emission file relied on role constants without directly including the LFG role definitions header.
- Recovery rule: when emitting RDF role payloads outside of existing queue action files, include the header that defines LFG role constants explicitly instead of assuming transitive visibility through manager headers.
- Narrow fix applied: add `#include "LFG.h"` to `RtgRdfQueuePlanner.cpp`; keep the existing `lfg::PLAYER_ROLE_*` usage unchanged.

## 2026-03-20 — Queue helper logout linker recovery

### Symptom
Worldserver link failed after the Phase 3 queue/RDF pass because `LfgActions.cpp` referenced two public queue-helper cleanup APIs that were declared in `RandomPlayerbotMgr.h` but had no matching definitions in `RandomPlayerbotMgr.cpp`:
- `RTG_ClearQueueHelperState(uint32, bool)`
- `RTG_RequestQueueHelperLogout(ObjectGuid, char const*, bool)`

### Root cause
The queue/RDF stabilization work had already started routing LFG mismatch cleanup through the public helper-logout interface, but the manager implementation still only had the lower-level private path `RTG_RequestSafeBotLogout(...)`.

### Narrow recovery applied
- added `RandomPlayerbotMgr::RTG_ClearQueueHelperState(...)`
- added `RandomPlayerbotMgr::RTG_RequestQueueHelperLogout(...)`
- kept the behavior narrow by reusing the existing manager event cache and the existing `RTG_RequestSafeBotLogout(...)` path instead of inventing a second logout flow

### Why this shape
This follows RTG brain doctrine:
- prefer narrow compile-safe recovery over broad refactors during active queue stabilization
- reuse the already-proven logout path
- keep helper queue cleanup semantics explicit and centralized in the manager

### Historical accounting
This was a **declaration/definition drift** failure, not a queue-planner logic failure. Future passes that expose new public manager hooks must confirm the `.cpp` implementation exists before packaging.

## 2026-03-20 — Phase 3 completion pass: finish-fill stall containment and helper headroom accounting

### Runtime evidence that triggered this pass
Recent WSG/EOTS/AB runtime logs showed a stable pattern:
- startup lanes filled correctly
- `live_refill` reached launch thresholds
- `finish_fill` continued to request more helpers
- later requests accumulated as `[RTG][DISPATCH][STALL]`
- follow-on passes repeated acquisition attempts even when no new offline queue-helper candidates were actually available

### What this pass changes
- dispatch-stalled helpers now clear through the existing `RTG_ClearQueueHelperState(...)` path instead of open-coding partial event resets
- BG acquire-side logic now measures whether any offline candidates remain for the requested team before attempting another helper admission
- RDF acquire-side logic now measures whether any offline candidates remain for the requested team/role before attempting another helper admission
- acquisition miss logging now records pending-helper count and busy-account count so account-pool pressure can be distinguished from planner defects

### Why this was the correct narrow repair
The queue planner was still emitting sensible demand. The broken surface was the boundary between demand and materialization:
- pending/stalled helpers were not being collapsed through the canonical helper-state cleanup path
- repeated acquire scans did not first prove that another eligible offline helper still existed

This pass therefore stays inside `RandomPlayerbotMgr.cpp` and reuses the already-established RTG queue helper cleanup surface instead of inventing a second retirement path.

### Historical accounting / lessons
- “need exists” is not the same as “an admissible offline helper exists right now”
- finish-fill loops can look like planner defects while actually being headroom starvation
- dispatch-stall cleanup must always route through the shared helper-state clearer so queue, retry, grace, and logout markers decay together
