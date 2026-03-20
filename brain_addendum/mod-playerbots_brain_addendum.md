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

## 2026-03-17 — Phase A/B/C pass: stale pending cancellation + arena scaffolding + dispatch audit

### Symptom
After the account-pool repair, the system could reliably hit **minimum battleground fill** with fair adaptive levels and immediate queue-helper gearing, but `live_refill` and especially `finish_fill` still stalled. Logs showed:

- healthy startup acquisition and login success
- later `finish_fill` demand remaining non-zero on one lane
- repeated `[RTG][DISPATCH][STALL]` on helpers reserved for lanes that had already become satisfied
- elevated busy-account counts despite large configured account pools

### Root cause
The remaining choke point was **stale pre-login helper reservations**. Queue-managed helpers that had already been requested for queue 3 / queue 4 were allowed to remain reserved until the generic stall timeout even after planner demand for those specific teams had dropped to zero. Those stale pending helpers continued to consume:

- busy accounts
- tracked pending-helper slots
- shared dispatch budget attention

This created a false scarcity pattern during `live_refill` / `finish_fill`, where the lane that still needed bodies could not quickly reclaim capacity already reserved by no-longer-needed pending helpers.

### Phase A fix
Add **demand-aware pre-login cancellation** for queue-managed pending requests in `ProcessBot(uint32 bot)`.

Offline helpers with RTG queue-managed add data are now revalidated against current planner demand **before** waiting for the stall timeout. If their lane no longer has demand for that request, the reservation is canceled immediately instead of waiting for the generic timeout.

This preserves the stall timeout as a fallback, but removes stale-pending starvation when one battleground lane reaches target before another.

### Phase B scaffolding
Add **arena lane scaffolding** without prematurely forcing arena helper materialization.

The battleground queue scan now computes arena-side RTG demand telemetry:

- `rtg_arena_need:<queue>:<bracket>`
- `rtg_arena_phase:<queue>:<bracket>`
- `rtg_arena_need_total`
- `rtg_arena_any_real_demand`

This is scaffolding only. It creates an explicit third RTG service-lane visibility surface so arena demand can be tracked separately from battleground demand, which is required before safe full arena integration.

### Phase C audit pass
Extend dispatch budgeting with a clearer shared-lane audit:

- pending LFG helpers
- pending BG helpers
- pending Arena helpers
- LFG need total
- BG need total
- Arena need total
- lane caps for BG and Arena startup/live/finish phases

This makes it much faster to distinguish:

- real finish-fill starvation
- stale pending helper backlog
- per-lane cap suppression
- future arena-vs-BG budget contention

### Doctrine note
This pass follows the safer RTG path:
1. remove stale pending starvation first
2. expose arena as its own lane in telemetry/scaffolding
3. strengthen shared dispatch observability
4. only then move toward real arena helper acquisition

That preserves the queue-lane separation doctrine and avoids reintroducing the same backlog problem into a future arena lane.

## 2026-03-18 — Arena lane sync repair: LFG bleed-through + arena target integration + team-side anchoring

### Symptom
Latest arena testing showed three connected failures:

- helpers could come online from the **LFG lane** even when the player was only testing arena queues
- arena demand existed, but the global RTG population target still only sized itself from **LFG + BG**, so arena materialization was not fully synchronized with the controller math
- regular arena helpers could be reserved for one side in logs but then behave as if they were attached to the wrong side during login / queue progression, which is unsafe for faction-compatible arena entry and same-faction handling

### Root cause
This was not one bug. It was a lane-sync defect across three layers:

1. **LFG demand creation** still accepted owners who were already in battleground/arena states, which allowed stale RDF ownership to keep spawning helpers during pure arena testing.
2. **Arena demand** was exposed in telemetry/scaffolding, but the RTG control target still only consumed `rtg_lfg_need_total` and `rtg_bg_need_total`, leaving arena outside the main population-sizing formula.
3. **Arena helper reservation / logging** still recorded the helper's actual faction-side instead of the desired assigned side from add-data, which blurred the distinction between:
   - desired arena side
   - actual character faction
   - queue-side ownership / retirement logic

### Repair
This pass tightens the arena lane without rewriting the planner:

- suppress LFG demand creation for real players already in BG/arena/BG-queue state
- fold `rtg_arena_need_total` into the RTG controller target calculation so LFG/BG/Arena lanes stay synchronized under one ceiling
- normalize custom arena backend sizes consistently in both planner math and direct-join logic
- record and log **desired arena side** at helper login / reservation time rather than only the bot's current faction side
- hard-abandon regular arena helpers whose actual faction side does not match the desired assigned side, so the system replaces them instead of letting mismatched helpers poison match assembly

### Intent
- stop stale RDF demand from bleeding into arena-only test sessions
- make arena demand visible to the same population controller used by BG/RDF
- keep helper-side ownership consistent enough for real-player arena entry to remain compatible
- preserve the doctrine that arena should become a first-class RTG service lane rather than a half-detached battleground shadow lane


---

## 2026-03-18 — Queue follow-up: stale RDF pending + penalty cleanup

### Summary
Follow-up pass after the queue lifecycle stabilization work.

### Added / changed
- RDF pending helpers now age out instead of renewing forever when they never fully enter queue/group ownership.
- Exact-role RDF helpers retry faster after login so role-correct fills materialize with less standby delay.
- Queue penalty cleanup was broadened to include additional dungeon deserter/cooldown variants.
- Assigned RDF helpers now refresh queued role state earlier before join when desired role data exists.

### Why
Live testing still showed:
- leftover bots online after queue work ended
- RDF helpers standing by too long before actually entering queue state
- stale account pressure after missed/partial queue cycles

### Expected result
Cleaner helper retirement, faster exact-role RDF convergence, and fewer busy random-bot accounts held by stalled queue helpers.

## 2026-03-19 — Arena orphan-demand collapse and helper retirement tightening

### Summary
Follow-up pass focused on the specific arena symptom where helper bots could remain online after all real arena demand was gone.

### Symptoms observed
- helper bots could stay online after arena testing ended even though no characters remained queued
- arena lane demand could stay alive from stale arena activity surfaces longer than real-player ownership justified
- arena cleanup could miss alliance-side mismatches because desired team `0` was being treated like an unset value

### Root cause
The arena scaffolding was still permissive enough to treat non-owner arena activity as live demand. In practice that meant bot-only residue could keep the lane warm long enough to delay retirement. Separately, the battleground/arena cleanup path used a truthiness check on `desiredTeam`, which silently skipped mismatch detection for team `0`.

### Implemented correction
- tighten arena scaffolding so `hasRealDemand` is driven by **real players or active arena instances**, not raw arena queue flags alone
- scale arena demand by `matchesNeeded` and `targetTotal` instead of one flat match target, so simultaneous arena formations have a clearer control surface
- emit explicit `[RTG][ARENA][ORPHAN]` breadcrumbs when bot-only arena residue remains without a real owner
- emit `[RTG][ARENA][OWNER]` breadcrumbs during helper cleanup to show whether a helper is still protected by real demand or lifecycle ownership
- fix cleanup-side team mismatch handling so desired team `0` is treated as a valid target side and not ignored

### Intent
- let arena demand collapse quickly once the real owner disappears
- reduce leftover arena helpers that linger for long periods after queue tests
- preserve the queue-ownership doctrine while making arena residue visible in logs instead of guesswork

### Files modified in this revision
- `src/Bot/RandomPlayerbotMgr.cpp`

### Expected result
- arena helpers should retire more reliably once the lane becomes bot-only residue
- simultaneous arena demand should size more clearly through `matchesNeeded` / `targetTotal`
- team-side cleanup should no longer silently ignore alliance-side mismatches


## 2026-03-20 — Queue stabilization checklist pass: explicit helper-state clearing + fast orphan retirement

### Intent
Tighten the queue-system checklist into direct implementation surfaces so the control plane stops carrying stale helpers after arena/BG/RDF tests.

### Manual checklist executed
1. **Single helper-state clearing surface**
   - add a shared `RTG_ClearQueueHelperState` / managed-event clearing path
   - use it for logout-adjacent queue cleanup instead of scattered partial clears

2. **Safe logout while session is already closing**
   - if a helper is already in logout/remove state, still clear RTG queue ownership state and remove it from current helper accounting
   - this prevents controller/account pressure from being held alive by helpers that are technically on the way out

3. **World-idle orphan retirement**
   - if a BG/arena helper is no longer lifecycle-owned, not in queue, and no longer backed by real demand, mark it for fast safe retirement
   - emit explicit arena orphan breadcrumbs for that condition

4. **LFG mismatch-join cleanup**
   - role-mismatch-on-join now uses the shared RTG queue-helper logout path instead of partial local event clearing
   - this keeps mismatch helpers from lingering online as half-owned queue residue

### Why this matters
Recent live tests showed three repeated failure signatures:
- a few helpers could remain online after all queues ended
- helpers already mid-logout could still hold queue state/account pressure longer than needed
- RDF role-mismatch joins could clear some state locally but still leave a helper in an unhealthy partial lifecycle

### Expected result
- fewer lingering helpers after arena/BG/RDF testing
- more reliable account unbusy behavior during queue collapse
- faster retirement of world-idle orphan arena/BG helpers
- cleaner RDF mismatch replacement without online residue

## 2026-03-20 — Phase 3 pass: RDF planner extraction + shared role resolver

### Summary
This pass begins the Phase 3 checklist by moving RDF helper-demand policy and talent/spec role interpretation out of the main stock-heavy files and into dedicated RTG overlay surfaces.

### What was changed
- added `src/Bot/RtgRdfQueuePlanner.h/.cpp`
  - moves RDF owner-demand timing, helper-need calculation, and global `rtg_lfg_*` event publication into a dedicated planner surface
  - keeps raw LFG observation in `RandomPlayerbotMgr::CheckLfgQueue()`
- added `src/Bot/RtgRdfRoleResolver.h/.cpp`
  - centralizes spec-tab to role resolution
  - centralizes offline spec-role inspection used during helper acquisition / role matching
  - centralizes queued-role normalization and role-target counts
- updated `src/Bot/RandomPlayerbotMgr.cpp`
  - `CheckLfgQueue()` now performs raw queue observation and then hands demand publishing to `RtgRdfQueuePlanner`
  - legacy inline RDF role helpers now route through the shared resolver surface instead of remaining duplicated in the stock-heavy file
- updated `src/Ai/Base/Actions/LfgActions.cpp`
  - LFG action role verification now uses the shared RDF role resolver instead of its own duplicated spec-role mapping logic

### Why this matters
Recent RDF queue investigations showed that queue behavior had become difficult to reason about because:
- role truth lived in more than one file
- RDF helper demand publication lived inline inside `RandomPlayerbotMgr.cpp`
- live fixes were increasingly mixing observation, policy, and lifecycle handling together

This extraction reduces that entanglement and makes future RDF timing/role fixes less likely to regress unrelated queue surfaces.

### Scope guard
This is a structural extraction pass, not a claim that RDF acceptance/materialization is fully solved.
It intentionally preserves:
- stock/raw LFG observation in `CheckLfgQueue()`
- existing RTG ownership/event semantics
- existing helper login / lifecycle machinery outside the planner boundary

### Expected result
- easier future debugging of RDF helper-demand publication
- one authoritative spec/role interpretation surface for RDF helper decisions
- less drift between helper acquisition role checks and in-action LFG role checks
