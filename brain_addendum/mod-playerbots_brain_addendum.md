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


## 2026-03-20 — Phase 3 acquire-side role enforcement packaging recovery

### Intake reason
A partial helper-only replacement of `RandomPlayerbotMgr.cpp` was packaged during Phase 3 follow-up work. That replacement began at helper function content instead of preserving the full translation unit, which produced immediate compile failures at file scope (`uint32`, `Player`, `sRandomPlayerbotMgr`, and `lfg::*` all appeared undeclared because the normal includes and surrounding context were missing).

### Root cause
This was not a logic regression in the RDF role-enforcement work itself. It was a **delivery-shape regression**:
- a fragment patch was handed back where the user needed a full drop-in source file
- the fragment started before the manager includes / namespace context
- subsequent local merges created misleading compile symptoms that looked larger than the underlying issue

### Recovery rule
For `RandomPlayerbotMgr.cpp`, RTG packaging must return the **full updated file** whenever the modified region sits inside shared helper scope or near high-risk manager glue. Do not return a headerless fragment and expect manual grafting when the active phase is already in compile recovery.

### Stable Phase 3 state being preserved
The drop-in manager file preserved here keeps the safer, already-integrated RDF acquisition shape:
- RDF owner buckets compute explicit tank/heal/dps demand
- role-demand events are emitted with `lfg::PLAYER_ROLE_*`
- acquisition only logs helpers whose offline spec role matches the requested RDF role
- assigned / queued counters are incremented role-faithfully so overfill pressure is reduced before queue join

### Historical accounting
This incident is recorded as a **packaging-form regression**, not a planner-doctrine regression. Future GPT passes should verify that any file handed back for `RandomPlayerbotMgr.cpp` begins with the original copyright block and include list before shipping it.
