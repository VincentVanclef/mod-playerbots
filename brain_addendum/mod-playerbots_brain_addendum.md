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

## 2026-03-20 — Phase 3 follow-up: event-driven helper account sizing and RDF starvation recovery

### Runtime evidence that triggered this pass
Fresh live logs showed three tightly related symptoms:
- one active BG lane could fill its minimum shell, but follow-on BG lanes repeatedly stalled at `pop_or_invite` or `finish_fill`
- RDF demand could coexist in the logs but never materially advance while BG demand remained active
- repeated `Can't log-in all the requested bots` errors appeared even though `RandomBotAccountCount = 1200`

### Root cause findings
This pass identified two narrow logic defects:

1. **RTG helper account sizing was still using legacy chars-per-account math**
   - legacy random-bot sizing divides by `CalculateAvailableCharsPerAccount()` because one account can supply different characters over time
   - RTG queue helpers do **not** work that way during a live queue window: the system intentionally treats one logged-in helper as consuming one account until the helper retires
   - result: only ~`eventMaxBots / charsPerAccount` accounts were being assigned to the RNDbot helper pool, which artificially capped multi-lane queue throughput

2. **Queued RDF admission was being suppressed whenever any BG demand existed**
   - the RTG acquire loop skipped non-active-dungeon RDF buckets whenever BG demand was present
   - result: RDF queue fill could be starved indefinitely by simultaneous BG demand even when lane capacity existed for both

### What changed
- RTG event-driven auto-sizing now derives helper-account demand as **one account per helper ceiling**, not chars-per-account
- RTG RNDbot account assignment now uses the same one-account-per-helper rule in event-driven mode
- RTG missing-account diagnostics now report helper-account shortage using divisor `1` in event-driven mode so the error reflects real queue-helper economics
- RDF reserved/shared-lane acquisition no longer suppresses queued RDF buckets merely because BG demand exists elsewhere

### Why this is the correct narrow repair
This stays inside the queue-helper admission/account-pool boundary instead of broadening planner behavior:
- the BG/RDF planners were still emitting valid demand
- the real break was helper materialization capacity and an over-aggressive RDF suppression guard
- fixing account-pool sizing plus the RDF starvation gate directly addresses the observed live symptoms without rewriting planner doctrine

### Historical accounting / lessons
- RTG helper pools must be sized by **concurrent helper sessions**, not by stored characters per account
- legacy random-bot account math is not safe to reuse blindly inside standalone queue-helper mode
- global BG pressure should influence capacity sharing, but it must not hard-disable queued RDF admissions

## 2026-03-21 — Phase 3 planner-authority conversion: RDF demand publishing moved to `RtgRdfQueuePlanner`

### Intake reason
The project reached a split-brain Phase 3 state:
- `RtgRdfQueuePlanner.cpp` and `RtgRdfRoleResolver.cpp` existed
- `LfgActions.cpp` was already consuming the resolver surface
- `RandomPlayerbotMgr.cpp` still owned RDF demand publication inline

That meant the new RDF planner translation unit existed but was not authoritative, which violates the RTG extraction checklist for Phase 3.

### Manual conversion performed
- added `#include "RtgRdfQueuePlanner.h"` to `RandomPlayerbotMgr.cpp`
- added `#include "RtgRdfRoleResolver.h"` to `RandomPlayerbotMgr.cpp`
- converted the local RDF role/spec helper layer in `RandomPlayerbotMgr.cpp` into thin wrappers over the shared `RTG::...` resolver functions instead of maintaining a second competing truth surface
- replaced the inline RDF demand-event publication block inside `RandomPlayerbotMgr::CheckLfgQueue()` with a single authoritative call to:
  - `RTG::RtgRdfQueuePlanner::ApplyDemandEvents(*this, requests, anyRealLfgDemand)`
- upgraded the local LFG owner snapshot usage in `CheckLfgQueue()` to use `RTG::RtgLfgQueueOwnerSnapshot`

### Why this shape was chosen
This follows the brain’s Phase 3 doctrine directly:
- keep raw LFG observation in `CheckLfgQueue()`
- move RDF demand policy into `RtgRdfQueuePlanner`
- keep role truth outside the stock file in `RtgRdfRoleResolver`

It also avoids a broad queue rewrite. Only the authority boundary was changed.

### Historical accounting / assumptions corrected
Previous recovery passes treated the missing piece as header hygiene or compile drift. Inspection of the uploaded module proved the deeper issue was architectural: the planner file existed, but the manager still published RDF events inline. This entry records that the real repair was an authority conversion, not another compile hotfix.

## 2026-03-21 — Phase 3 follow-up: planner hookup completion and pending-ledger pressure correction

### Runtime evidence that triggered this pass
Fresh queue testing showed three linked symptoms:
- RDF helper bots could log in but never materially advance into queue participation
- battleground follow-up fills were stalling after the first lane, especially once multiple queue families overlapped
- arena queues still had almost no RTG planner breadcrumbs because the planner path was skipping arena queue families entirely

### Root cause findings
This inspection found three narrow integration defects:
1. `RtgBgQueuePlanner` was still skipping arena queue families, so arena demand had almost no planner-event/log surface.
2. RTG pending-helper accounting still treated `WorldIdle` ledger rows as in-flight pressure, which overstated pending helpers and lane pressure.
3. RTG busy-account reconstruction was still counting any non-retired ledger row as account pressure, which let stale offline helper rows artificially block later queue admissions.

### Manual corrections applied
- `RandomPlayerbotMgr.cpp`
  - added normalized arena team-size helper for queue planning surfaces
  - stopped skipping arena queues when building adaptive BG/arena queue observations for RTG helper admission
  - changed pending-helper accounting so `WorldIdle` no longer counts as active pending pressure
  - changed busy-account reconstruction to count only truly pending helper ledger states
  - upgraded ownership audit to remove stale offline helper ledger rows that no longer have `add`/`logout` truth behind them
- `RtgBgQueuePlanner.cpp`
  - added normalized arena team-size logic
  - added planner/event emission for arena queues using `[RTG][ARENA][PHASE]`, `[RTG][ARENA][DEMAND]`, and `[RTG][ARENA][CLEAR]`
  - publishes shared `rtg_bg_team_need:*` and phase events for arena queue families so the stock orchestrator can consume them the same way it does battleground demand

### Why this is the correct narrow repair
The newest symptoms were not solved by more RDF role logic alone. The real break sat in the planner/materialization seam:
- arenas were still largely outside the planner breadcrumb surface
- stale helper ledger rows were inflating pending and busy-account pressure
- later lanes were being blocked by accounting truth that no longer reflected real helper state

This pass therefore tightens the authority boundary instead of broadening queue doctrine.

## 2026-03-21 — Queue-system continuation journal: RDF dispatch + multi-queue correction
- Confirmed RDF was still weaker than BG at the manager/dispatch boundary: acquire/login existed, but there was no equivalent of the BG immediate queue-dispatch helper. That is why logs could stop at acquire/add with little proof of actual queue participation.
- Confirmed acquire-side phase mapping had drifted away from planner truth. This was a quiet multi-queue bug because wrong phase ordering/suppression can make one lane appear “dominant” while others remain starved.
- Confirmed one shared-surplus RDF suppression guard still existed despite prior starvation-recovery intent.
- Repaired all three surgically and expanded breadcrumbs only at the proving surfaces: RDF join/accept/fail, BG multi-queue/assign, Arena form/pop, and generic dispatch success/failure reasons.

## 2026-03-28 — RDF proposal loop containment and true final-dialog seam correction

### Runtime failure that forced rollback
The last RDF proposal patch had to be reverted after live testing showed a catastrophic repeat loop:
- pressing the final Dungeon Finder ready/enter flow caused the same proposal-related packet path to fire repeatedly
- player session traffic spiraled hard enough to destabilize worldserver
- host pressure became large enough that the website/domain on the same machine also became unresponsive

### What was assumed incorrectly
Two assumptions were wrong at the same time:
1. The proposal packet could be safely acted on immediately from the raw outgoing-packet hook.
2. Repeated `SMSG_LFG_PROPOSAL_UPDATE` packets could be treated like fresh accept commands instead of proposal-state refreshes.

Those assumptions created a dangerous architecture seam: packet observation and proposal consumption were happening in more than one place, with no one-shot guard per proposal id.

### Real core-side owner seam
AzerothCore LFG still owns proposal resolution. The bot layer should only:
- parse/store the active proposal truth
- send `CMSG_LFG_PROPOSAL_RESULT` once for a given proposal id while the proposal state is still `LFG_PROPOSAL_INITIATING`
- then stand down until core advances proposal state

That means proposal handling must be split cleanly:
- `PlayerbotAI` packet hook = observe/cache state only
- `LfgAcceptAction` = one guarded acceptance path only
- RTG queue manager = wait/suppress replacement churn while proposal is unresolved

### Manual repair applied in this pass
- `PlayerbotAI.cpp`
  - stopped directly sending `CMSG_LFG_PROPOSAL_RESULT` from the outgoing packet hook
  - now parses the real packet layout (`dungeonEntry`, `state`, `proposalId`, `encounters`, `silent`, `groupSize`)
  - caches proposal id only while state is `LFG_PROPOSAL_INITIATING`
  - clears cached proposal state on non-initiating updates
  - clears RTG proposal lifecycle markers on explicit proposal failure
- `LfgActions.cpp`
  - added safe manual parsing for `SMSG_LFG_PROPOSAL_UPDATE`
  - moved acceptance to a single guarded path
  - added `rtg_lfg_accept_proposal` marker so the same proposal id is only accepted once per helper
  - suppresses repeated identical proposal updates instead of re-sending accept forever
  - only accepts while proposal state is still `INITIATING`
- `RandomPlayerbotMgr.cpp`
  - queue helper lifecycle cleanup now also clears `rtg_lfg_accept_proposal`
  - login-error / helper-state cleanup clears the same guard so stale helpers do not poison later proposals
  - comment/doctrine updated so packet observation no longer pretends to own the Enter Dungeon button path

### Why this is the safer architecture
This pass does **not** broaden RTG ownership. It removes duplicate ownership.

RTG should not press proposal accept from both the packet hook and the action layer. That double-surface is what made repeated proposal updates dangerous. By collapsing acceptance into one guarded action and leaving packet hook code as observer-only, repeated update packets become informational instead of destructive.

### Proof signals required next
A healthy live test should now show:
- one `[RTG][RDF][ACCEPT]` per helper per proposal id
- repeated `SMSG_LFG_PROPOSAL_UPDATE` lines may still appear, but they should produce only `[SUPPRESS]` breadcrumbs for already-handled proposal ids
- no packet flood after the ready dialog appears
- no host-wide overload when the final RDF dialog stage occurs
- no extra helper acquisition while proposal is still actively resolving

### Failure signals to watch for immediately
Abort and rollback if any of the following reappear:
- repeated accept lines for the same helper/proposal id
- the same proposal packet logged continuously with no state change
- worldserver CPU or log rate spikes immediately after proposal update / enter-dungeon stage
- replacement helper acquisition beginning before proposal failure is explicit

## 2026-03-28 — RDF group-owner normalization + guarded post-group teleport recovery

### Live test findings that drove this pass
- proposal packet safety guards held: no repeat-packet storm, no worldserver/domain overload
- RDF proposal still proved timing-sensitive and expired once before a successful second ready cycle
- after successful group materialization, at least one healer and one DPS helper remained online but did not enter the dungeon
- refill demand then began from a new owner key (`...owner=1`) while the original helpers were still carrying their old player-owner add-data (`...owner=6004`)

### Incorrect assumptions corrected
1. We treated `group->isLFGGroup()` as sufficient proof that helpers would complete the last-mile teleport on their own after proposal acceptance. In practice, helpers can miss the initial enter-dungeon moment and remain stranded online outside the dungeon.
2. We treated helper ownership as stable from initial player queue ownership through live LFG group materialization. In practice, the demand planner pivots to the active LFG group GUID as owner, while already-assigned helpers were still tagged to the original player GUID. That owner split can make healthy helpers disappear from bucket accounting and provoke false replacement pressure.

### Manual repair applied
- `RandomPlayerbotMgr.cpp`
  - added guarded normalization of RTG LFG helper `add` owner from original player owner to active LFG group owner once the helper is inside a real-player LFG group
  - added guarded post-group teleport recovery window for helpers who reached `group->isLFGGroup()` / `LFG_STATE_DUNGEON` but are still outside the dungeon map
  - teleport recovery is throttled with explicit one-shot/retry state:
    - `rtg_lfg_group_ready_since`
    - `rtg_lfg_teleport_sent`
    - `rtg_lfg_teleport_attempts`
  - capped automatic post-group teleport attempts and widened proposal-resolution patience so RTG does not immediately classify normal late transitions as failure
- `LfgActions.cpp`
  - proposal lifecycle cleanup now also clears the new post-group teleport markers
  - queued RDF teleport action now hard-stops combat before issuing `CMSG_LFG_TELEPORT`
- `PlayerbotAI.cpp`
  - explicit proposal-failure cleanup now also clears the new post-group teleport markers

### Why this owner/teleport pass is safer
This pass still keeps core LFG as the owner of actual group materialization and dungeon transfer. RTG only does three tightly-bounded things:
- keeps helper ownership aligned with the owner identity the demand planner is actually using
- retries the same legitimate `lfg teleport` action in a throttled, finite way when a helper reached live LFG-group state but missed the initial enter-dungeon moment
- suppresses stale replacement churn while that finite recovery window is active

### Proof signals required next
- after live RDF group formation, helpers should log `[RTG][RDF][OWNER] ... reason=lfg_group_normalize` when ownership migrates from player-owner to LFG-group-owner
- stranded helpers should log at most a small bounded sequence of `[RTG][RDF][TELEPORT]` / `[WAIT_TELEPORT]` lines rather than silently remaining outside forever
- no packet storm, no repeated unbounded teleport spam, no host overload
- refill acquisition should stop overcounting missing roles merely because existing helpers are still tagged to the obsolete pre-group owner id
- if a helper truly cannot enter after the bounded recovery window, the logs should now end in explicit `reason=teleport_not_resolved` rather than silently drifting into bad role math

## 2026-03-28 — RDF flex-role doctrine for ambiguous specs (safe feral dual-role pass)

### Live findings that drove this pass
- RDF queue safety improved enough to reach actual group formation and dungeon entry without the earlier proposal packet storm.
- New testing exposed a quieter but important queue-definition seam: a feral druid helper could be a valid tank or DPS choice for Wrath RDF, but RTG role resolution still tended to collapse that spec into a single role identity.
- That single-role collapse can hurt both acquisition and dispatch:
  - known feral offline specs can be skipped when DPS is requested because the resolver treated feral as tank-only
  - already-online feral helpers can be unnecessarily respecced away from feral for a DPS request even though feral already satisfies both tank and damage viability
  - queue join packets can advertise only one role even when the current spec is safely valid for more than one

### Incorrect assumptions corrected
1. We previously treated `RoleForClassSpecTab()` as sufficient for RDF queue viability. That is safe for single-role specs, but not for truly ambiguous specs like feral druid where one spec can legitimately queue as both tank and DPS.
2. We previously used `currentRole != desiredRole` as the respec trigger in helper preparation. That wrongly treats a valid flex-capable spec as a mismatch when the desired role is one of the spec's valid roles.
3. We previously treated a cached/known offline role as one exact lane rather than a capability mask. That meant known feral helpers could disappear from DPS candidate searches.

### Manual repair applied
- `RtgRdfRoleResolver.h/.cpp`
  - added explicit role-mask doctrine helpers:
    - `RoleMaskForClassSpecTab()`
    - `GetOfflineSpecRoleMask()`
    - `GetActualSpecRoleMask()`
  - codified the first safe flex-role rule narrowly and intentionally:
    - **druid feral = tank | damage**
    - restoration remains healer-only
    - balance remains damage-only
    - other classes/specs remain conservative single-role until proven safe with gear/spec doctrine
- `RandomPlayerbotMgr.cpp`
  - helper preparation now checks **role capability mask** instead of exact single-role equality before forcing a respec
  - known offline/cached candidates are now matched by `roleMask & desiredRole` instead of exact-role equality
  - runtime cache now stores both:
    - `rtg_runtime_lfg_role`
    - `rtg_runtime_lfg_role_mask`
  - dispatch mismatch breadcrumbs now report both `actualRole` and `actualMask`
- `LfgActions.cpp`
  - assigned RDF helpers whose current spec safely covers more than one role now queue with the safe role mask instead of an unnecessary single-role collapse
  - current feral helpers can therefore advertise `TANK|DPS` while still satisfying an explicit desired-role request
  - role breadcrumbs now include `actualMask` and a new `verdict=flex_mask_override` path

### Why this is the safer doctrine
This pass does **not** open the floodgates for every hybrid spec to multi-queue. It only formalizes the flex case that current live testing actually exposed and that fits the user's explicit doctrine:
- **feral druid can queue as tank and DPS without a spec swap**
- specs that normally require a true spec/gear identity change remain single-role for now

That keeps RTG aligned with AzerothCore/LFG expectations while avoiding premature broad multi-role theology.

### Proof signals required next
- when a feral druid helper is chosen for RDF, logs should show capability breadcrumbs containing `actualMask=10` (`TANK|DPS`) rather than treating feral as tank-only
- a known offline feral druid should now be selectable when RTG needs either:
  - a tank helper, or
  - a DPS helper
- a feral helper already in a valid feral spec should **not** be forcibly respecced to balance merely because the current request is DPS
- join logs for the feral helper should show `roleMask=10` and role text `TANK/DPS`
- no regression in healer-only specs or strict single-role specs

### Failure signals to watch for next
Rollback or tighten doctrine if any of the following appear:
- paladins/other hybrids begin queueing for roles that clearly require a full gear/spec identity change
- feral helpers begin being overcounted as if one bot fills both tank and DPS simultaneously in planner math
- repeated `runtime_role_mismatch` on helpers whose actual mask clearly included the requested role

## 2026-03-28 — RDF transition protection, stale-instance recovery, and immediate proposal consumption

### Live findings that drove this pass
- RDF proposal safety held and no longer flooded the server, but helpers still showed three practical failure modes:
  - LFG groups could form and then some helpers would still fail to enter, after which they appeared to leave party or disappear from the run.
  - Some helpers were being re-selected while still physically sitting inside an old dungeon map from a prior run, which made them look available to RTG even though they could not actually queue.
  - Proposal acceptance still felt sluggish in live testing even when the ready dialog was valid and safe to consume.
- The new logs also showed a false regression seam after `rdf_teleport` success:
  - helpers reached `group_ready`
  - then later emitted `WAIT_READY` / `proposal_not_resolved`
  - then rejoined as though proposal had never advanced
- Repeated `join_dispatch_failed ... dead=1` lines proved that dead helpers could keep getting shoved back through RDF join attempts instead of being recovered first.

### Incorrect assumptions corrected
1. We had an RTG group-dissolve policy that only considered an LFG group "allowed" once the helper was already inside an instance map. That is too strict for the real RDF transition window because a healthy LFG group can exist briefly outside the map while the core-side enter-dungeon handoff is still resolving.
2. We were still carrying proposal-accept markers forward even after the helper had already reached the later `group_ready` / teleport stage. That let an already-advanced helper get misclassified later as `proposal_not_resolved`.
3. We were treating old-dungeon sitters and dead helpers as ordinary queue candidates, which allowed repeated queue churn instead of recovery.
4. We were relying too heavily on packet-trigger timing for accept, even though the manager already had safe one-shot guards and could consume a visible proposal immediately once cached.

### Manual repair applied
- `RandomPlayerbotMgr.cpp`
  - added `RTG_HasActiveLfgTransitionState()` so RTG recognizes an active RDF transition window using proposal, teleport, pending, and dungeon-active markers
  - relaxed the anti-group policy for LFG groups outside the dungeon map **only** while that transition window is active, preventing helpers from being forced out of a healthy RDF party during the last-mile handoff
  - added `RTG_ClearProposalAcceptState()` so proposal-lock / accept markers are cleared as soon as a helper reaches `groupReadyForTeleport`, preventing later false `proposal_not_resolved` failure on an already-advanced helper
  - added `RTG_RecoverQueuedDungeonHelper()` to manually recover stale helpers before requeue:
    - resurrect dead helper if needed
    - clear combat/unit state
    - leave non-LFG groups if stuck in one
    - call `TeleportToEntryPoint()` when the helper is still sitting inside an old instance map
  - added a bounded fast path so when manager sees a cached live proposal id and no accept has been sent yet, it immediately calls the guarded `lfg accept` action (`reason=proposal_visible`) instead of waiting around for slower ambient action cadence
  - dead or stale-instance helpers are now recovered first and **not** pushed straight back into join dispatch the same tick

### Why this pass is safer
This pass still does not invent a fake dungeon-ownership system. Core LFG remains the owner of:
- proposal lifecycle truth
- actual group materialization
- actual dungeon transfer

RTG only does four bounded things:
- stops killing a legitimate LFG group during the transition window
- clears obsolete proposal-accept markers once later-stage group-ready truth exists
- recovers obviously stale or dead helpers before asking them to queue again
- consumes a visible proposal faster, but only through the already-guarded one-shot accept path

### Proof signals required next
- no helpers should silently leave or lose party simply because they are in an LFG group outside the map for a short transition window
- after `rdf_teleport reason=group_ready`, the same helper should **not** later fall back into `proposal_not_resolved` for that same advanced transition
- stale old-dungeon helpers should log `[RTG][RDF][RECOVER] ... reason=stale_instance` instead of waiting forever unavailable inside an old instance map
- dead helpers should log `[RTG][RDF][RECOVER] ... reason=dead_before_join` and stop spamming repeated `join_dispatch_failed ... dead=1`
- proposal acceptance should occur faster, with manager-side success breadcrumbs like `lane=rdf_accept reason=proposal_visible`, but still no packet storm

## Chapter 2026-03-28 — RDF orphan grace + priest main-tank shielding pass

### Situation
A live test finally produced the desired fast RDF materialization and clean dungeon entry, but two follow-up seams appeared immediately afterward. First, when the real player left the dungeon partway through the run, helpers stayed online as stragglers and kept trying to rejoin RDF under the normalized group owner (`owner=1`), producing repeated `join_dispatch_failed` loops instead of retiring cleanly after the run ended. Second, priest healer behavior was still too passive for level-19 dungeon pacing: shields were inconsistent and direct heals often started too late on the tank.

### Incorrect assumptions corrected
1. Owner normalization to the live LFG group solved refill accounting, but it also meant helper ownership could outlive the real-player demand signal. Without a separate orphan-handling rule, RTG kept treating post-run stragglers as queue candidates.
2. A helper that already completed an RDF run should not instantly be shoved back into join dispatch just because `rtg_dungeon_active` is still warm while the player has already left.
3. Priest shielding logic was too generic; it needed explicit main-tank preference rather than hoping party-wide target selection would choose the tank early enough.

### Manual repair applied
- `RandomPlayerbotMgr.cpp`
  - added `rtg_lfg_orphan_since` handling and a bounded 180-second orphan grace for helpers that are no longer in a group, no longer inside the instance, and no longer backed by live real-player RDF demand
  - orphaned helpers now stop trying to rejoin RDF during that grace window and are logged out cleanly after the grace expires via `RTG_RequestSafeBotLogout(..., true)`
  - stale queue sweeps now ignore helpers already marked orphaned so they do not churn through duplicate stale/idle retire paths while waiting out the grace period
  - queue-helper and lifecycle cleanup now clears the orphan marker so stale state cannot poison future assignments
- `PriestActions.cpp`
  - `power word: shield on not full` and `power word: shield on almost full health below` now explicitly prefer the `main tank` target first when shielding is legal and useful
- `HealPriestStrategy.cpp`
  - default heal-priest behavior now opens more proactively with `power word: shield on not full`
  - critical/low/medium party-health trigger priorities were shifted so shield, penance, and flash heal beat slower follow-up tools more often, improving first-response healing on tanks

### Proof to demand next
- after the real player leaves a successful RDF run, helpers should log an orphan breadcrumb and then idle out instead of rejoining RDF immediately
- there should be no repeated `owner=1 ... join_dispatch_failed` loops during the orphan grace window
- after roughly three minutes, straggler helpers should log out cleanly
- priest healers should apply `Power Word: Shield` to the tank earlier and begin direct healing sooner under sustained pressure

## Chapter 2026-03-28E — Stable RDF owner identity, assigned-role join discipline, disc flex doctrine, priest shielding, and summon-ground safety

### Context
After the successful RDF entry hardening pass, follow-up testing exposed a different family of seams: helpers could inherit synthetic owner ids such as `1` or `2` after LFG group materialization, second-character RDF tests could starve because demand/accounting no longer matched the initiating real player, flex-role feral join masks could interfere with refill behavior, Discipline priest capability still collapsed too conservatively, priests still under-used `Power Word: Shield`, and summon placement could land helpers below terrain when master-relative Z was unsafe.

### Incorrect assumptions retired
- LFG group GUID low values are **not** stable RTG queue owners.
- A flex-capable spec should not automatically advertise every safe role at queue-join time once RTG has already assigned a specific slot.
- Priest discipline should not be treated as healer-only for RTG helper capability matching.
- Summon target Z should not assume the master's raw Z is safe terrain for the bot destination.

### Manual changes
1. Added a stable real-player owner resolver and used it for RDF owner normalization and real-player RDF bucket construction.
2. Stopped flex-capable assigned helpers from advertising a multi-role join mask when RTG has already assigned a concrete role. Flex capability is still used for candidate selection/prep, but queue join now stays slot-disciplined.
3. Extended Discipline priest role capability to `HEALER|DAMAGE` while keeping Holy healer-only and Shadow damage-only.
4. Raised priest shielding pressure around main-tank protection and near-full-health pre-shield windows.
5. Grounded summon destinations with terrain height updates before teleport.

### Expected proof
- No follow-up RDF helper accounting under owner ids `1` or `2` when the real queue owner should be the initiating real player.
- Second-character RDF tests should acquire and dispatch under the correct player owner id.
- Feral and Disc helpers can still satisfy either safe role during candidate selection, but once assigned they queue as the requested slot only.
- Priests should begin shielding tanks much earlier, including pre-shield windows when `Weakened Soul` is absent.
- Summoned helpers should stop falling below terrain from bad Z inheritance.

## Chapter 2026-03-30-C — RDF owner drift pinning + strict paladin queue doctrine
- Context:
  - Follow-up live RDF testing reported that a holy paladin still ended up in the tank slot, and a second-character RDF test later drifted into low synthetic owners like `1` / `2` instead of remaining attached to the initiating player.
- Incorrect assumption retired:
  - Normalizing helper ownership forward into the live LFG group would remain equivalent to the real player owner.
  - In practice this polluted refill/orphan/account-capacity math and allowed later queue work to key off synthetic group ids.
- Real seam:
  - RTG helper add-data owner must stay pinned to the initiating real player.
  - Live group state can inform transition handling, but it must not rewrite queue ownership identity.
- Code changes:
  - `RandomPlayerbotMgr.cpp`
    - stopped rewriting RDF helper `add` ownership during LFG-group normalization; the system now retains the original real-player owner and only emits a breadcrumb.
    - added a stricter runtime mismatch rule for paladins so assigned RDF role must equal actual runtime role before dispatch proceeds.
  - `PriestActions.cpp` / `HealPriestStrategy.cpp`
    - increased pre-emptive shielding priority and added a support-target preference path so priests protect the main tank/master sooner.
- Intended proof:
  - second-character RDF should continue to use the real player owner instead of drifting to `1` / `2`.
  - holy paladins should no longer survive dispatch into a tank assignment; they should be rejected and recycled instead.
  - priests should apply `Power Word: Shield` sooner, especially on main tank / supported player targets.

## Chapter 2026-03-30 — Multi-owner dispatch floor
- Situation: first owner could receive helper requests and logins, while later owners/lane requests stayed stuck in add-data and emitted repeated DISPATCH STALL lines.
- Finding: RTG queue-managed helpers already present in currentBots could outpace the planner's global need snapshot, letting the online/login budget fall below already-requested queue helpers.
- Correction: added a managed-floor rule in RandomPlayerbotMgr so rtgEventDriven maxAllowedBotCount never drops below the number of already-tracked queue-managed helpers.
- Proof target: second and later RDF/BG owners should now produce DISPATCH ADD / LOGIN instead of stalling forever behind the first owner.

## Chapter — Multi-owner pending-login floor
- Situation: two RDF groups could materialize at the same time, but a third queue would only reach `ACQUIRE][REQUEST]` and then stall in pending/add-data for 40+ seconds.
- Finding: the login stage still budgeted against visible online helpers strongly enough that already-requested pending helpers were not guaranteed dispatch/login room while earlier groups remained active.
- Correction: reserve login headroom for `pendingQueuedLogins` by forcing `maxAllowedBotCount >= onlineBotCount + pendingQueuedLogins` during RTG event-driven dispatch budgeting.
- Proof target: after two active RDF groups exist, a third queue should move from `ACQUIRE][REQUEST]` to `DISPATCH][ADD]` / `LFG][LOGIN]` instead of accumulating `DISPATCH][STALL]`.


### 2026-03-30 — RDF druid spec-role hardening
- Locked druid RDF offline-role resolution to strict spec doctrine when spec data exists: Balance = DPS only, Feral = Tank/DPS, Restoration = Healer only.
- Added a conservative fallback for druids when offline talent/spec truth is unavailable: fallback advertises Balance/DPS only instead of allowing stale runtime caches to surface tank or healer capability.
- Purpose: stop Restoration druids from ever being selected into RDF tank duty due to unknown/offline-role ambiguity.


## Queue follow-up: spec-role doctrine and combat-role override
- Locked RDF role doctrine to the user-specified class/spec map for current resolver assumptions: priest disc=heal/dps, holy=heal, shadow=dps; warlock/mage/rogue/hunter always dps; druid resto=heal, feral=tank/dps, balance=dps; warrior prot=tank arms/fury=dps; paladin holy=heal prot=tank ret=dps; shaman resto=heal ele/enh=dps.
- Identified a second seam after queue-role fixes: combat behavior was still driven mainly by spec-default strategies, especially for ambiguous specs like Discipline priest and any bad runtime cases where a healer spec slipped into a DPS queue role.
- Added LFG role-aware combat override so bots in live LFG groups derive tank/heal/dps behavior from the assigned RDF role first, rather than only the default spec strategy.
- Extended default combat strategy override to support LFG damage roles explicitly, including Disc priest DPS, healer-spec fallback DPS behavior, and removal of healer/tank strategy residue when the assigned role is damage.
- Proof target: a Disc priest assigned DPS should behave as DPS in the dungeon; a healer-spec bot that somehow reaches a DPS slot should still contribute damage instead of standing in healer behavior.

## Chapter — RDF demand must subtract already-assigned helpers
- Situation: two simultaneous RDF groups could form, but a third owner stalled in `ACQUIRE][REQUEST]` / `DISPATCH][STALL]` while the planner kept claiming more accounts were needed.
- Root seam: global RDF demand was still computed from real-player role counts only. Already-assigned/queued RTG helpers for active owners were not subtracted from owner demand, so completed/active runs still contributed phantom helper need.
- Correction: owner snapshots now include queued vs assigned helper role counts from current managed helper add-data, and the RDF planner subtracts those counts before publishing `rtg_lfg_need_*` / `rtg_lfg_need_total`.
- Proof target: after two live RDF groups exist, a third owner should only request its own four helpers instead of inheriting phantom demand from the already-filled runs.

## Chapter — Live LFG role strategy reset
- Situation: bots could queue into the correct RDF role but still fight like their previous healer-spec strategy after the live LFG group formed.
- Root seam: strategy overrides were present, but some helpers never rebuilt combat strategies after the final LFG role became visible on the live LFG group.
- Correction: once a helper is inside a live LFG group / proposal-visible / teleport-ready state, RTG now resets strategies one time per live role change and stores `rtg_lfg_strategy_role` so healer residue does not persist into a DPS assignment.
- Proof target: Disc priest DPS should switch into damage behavior once the live LFG role is visible; healer-spec residue should not survive into a DPS slot.


## Chapter 2026-03-31 — RDF Owner Lane Closure
- Symptom: once an RDF owner had a live dungeon run, later owners could stall because the older owner still held open a global RDF request lane.
- Finding: active dungeon owners were still contributing to global RDF helper demand even though their group was already filled and running.
- Fix: close the owner request lane when the RDF group is already materialized (`activeDungeon`) or no helper roles remain needed. Keep helpers online for the run, but stop that owner from reserving future RDF fill capacity.
- Proof target: after one owner enters a live dungeon, a later owner should be able to open a fresh RDF request and receive new helper logins.


## Chapter 2026-04-03 — Queue lane login starvation seam
- **Situation:** Later RDF/BG/arena owners could acquire helpers and write add-data, but those helpers never came online unless they were requested in the same initial timing window as the first owner.
- **Root cause:** The login phase was only iterating `availableBots`. Queue-managed helpers already acquired into `currentBots` with add-data were not guaranteed to still be present in `availableBots`, so later-owner helpers could sit offline until dispatch-stall cleanup.
- **Correction:** Process offline queue-managed `currentBots` first during the login phase, then fall back to `availableBots`. This preserves multi-owner service across sequential RDF/BG/arena requests while earlier owners remain active.
- **Proof target:** Owner A can already have active helpers online, and Owner B/Owner C can still move from `ACQUIRE][REQUEST]` to `DISPATCH][ADD]` and `LOGIN` without needing to queue inside the same initial timing window.

## Patch focus

This pass fixes an RDF planner seam where helpers that were already assigned to an owner could accidentally cause that owner's demand lane to close before those helpers had actually entered the queue.

## Problem observed

Owner A could acquire and log in all four helpers, but every helper still failed `lfg join` with `join_dispatch_failed` while remaining in `LFG_STATE_NONE`. The planner had already counted `helperAssigned*` as satisfying the owner's role need, so `rtg_lfg_real_demand` dropped to zero too early. Once that happened, `LfgJoinAction` treated the helper as having no active owner demand and blocked the queue join path, causing repeated retries and long activation delays.

## Formula correction

RDF owner accounting is now split into two truths:

1. **acquire need** = what still must be newly acquired/logged in after counting assigned helpers.
2. **lane need** = whether the owner's live RDF request must remain open so already-assigned offline helpers can still finish joining.

The owner lane now stays open until either:
- the run is already active in-dungeon, or
- queued/live role coverage is complete **and** there are no offline assigned helpers still outstanding.

## Intent

- keep owner demand alive long enough for assigned helpers to actually queue
- stop `helperAssigned` from prematurely closing the owner's request lane
- preserve non-duplication of helper acquisition while fixing Owner A join starvation


## Chapter: RDF login role enforcement bridge
- Context: Bots could enter RDF with correct assigned queue roles but still behave according to spec-default strategies until a live LFG group role became visible.
- Finding: The missing seam was between login/assignment and combat-role application. `rtg_lfg_strategy_role` existed, but live enforcement only occurred after an LFG group materialized.
- Change: Added login-time RDF role enforcement in `RandomPlayerbotMgr::OnBotLoginInternal` and extended `PlayerbotAI::IsTank/IsHeal/IsDps` to honor `rtg_lfg_strategy_role` while `rtg_lfg_pending` or `rtg_dungeon_active` is set.
- Effect: Assigned RDF roles now influence AI identity immediately at login and remain aligned through pending/active dungeon states instead of falling back to healer/spec defaults.
- Proof target: Logs should show `[RTG][RDF][ROLE_APPLY]` at login and bots in RDF DPS slots should stop acting like healers before or during queue materialization.

## Chapter: Conservative hybrid fallback and orphan disposal tightening
- Context: Sequential RDF owners started forming, but some later groups still surfaced obviously wrong hybrid assignments (for example fury-style tanks or enhancement-style healers), and disposed dungeon helpers could linger after the real player had already left and taken deserter.
- Finding: The remaining ambiguity lived in two seams. First, when offline spec truth was unavailable, hybrid candidate selection could still consult stale runtime caches and advertise tank/healer capability that should not be trusted. Second, orphan cleanup used a grace window even for helpers already disposed from a dungeon run.
- Change: Hardened offline hybrid fallback so druids, paladins, priests, shamans, warriors, and death knights now fall back to the conservative class-default spec mask instead of stale runtime caches when offline spec truth is unavailable. Also changed orphan cleanup so disposed helpers with deserter/dungeon-deserter immediately log out instead of waiting through orphan grace.
- Effect: Hybrid helpers should stop surfacing into obviously wrong queue roles when spec truth is uncertain, and disposed dungeon helpers should no longer linger after the real player has already abandoned the run.
- Proof target: Future bad-group scenarios should stop showing obvious role/spec contradictions for hybrid fillers, and logs should show immediate `rtg_lfg_disposed` logout after owner departure/deserter disposal.

## Chapter: Queue-role enforcement without generic strategy drift
- Context: Login-time RDF role enforcement helped bind assigned queue role into AI identity, but broad generic strategy rewrites could override class/spec combat packages and produce odd combat expression (for example Balance drifting into melee/bear-style behavior).
- Finding: The durable value was the queued-role event state itself (`rtg_lfg_strategy_role`) plus `PlayerbotAI::IsTank/IsHeal/IsDps` honoring it during pending/active dungeon states. The generic `+tank/+heal/+dps` rewrites were the risky part.
- Change: Role enforcement now preserves the queued-role event bridge and resets strategies, but no longer injects broad generic strategy swaps that can erase class/spec-specific combat logic.
- Effect: Queue role still propagates into AI identity, while class/spec combat packages remain intact instead of being replaced by overly generic role strategies.
- Proof target: Balance druids in RDF DPS roles should stop defaulting into bear/melee expression while still remaining classified as DPS for queue-role logic.


## Chapter - Strict tank/healer acquisition and login-time role validation
- Bootstrap fallback is now disabled for RDF tank/healer acquisition. Unknown offline-spec hybrids may still bootstrap into DPS, but tank and healer slots require reliable offline spec truth.
- Added login-time role validation mirroring dispatch-time validation so bad spec/role matches are rejected before entering the queue lifecycle.
- Extended exact-role doctrine to warrior, shaman, paladin, death knight, mage, warlock, hunter, and rogue. Druid flex remains feral only; priest flex remains discipline only.


## Chapter 44 — RDF role derivation doctrine hardening (2026-04-04)

### Context
Live RDF testing still showed severe role/spec mismatches, including Restoration druids surfacing as tank or DPS candidates. This proved the queue pipeline was still letting demand reshape bot specs instead of selecting bots whose real specs already matched the requested role.

### Finding
The main seam was `RTG_PrepareLfgHelperForDesiredRole()`. That function was still reinitializing talents/equipment to a preferred spec for the requested RDF role. In practice, that let the queue system coerce helpers into roles instead of treating queue role as a consequence of real spec truth. A second seam remained in offline selection: hybrid classes without offline spec truth could still be surfaced through fallback logic.

### Change
- Disabled hybrid fallback role exposure for RDF selection when offline spec truth is missing. Only pure DPS classes remain safe to expose without spec data.
- Changed `RTG_PrepareLfgHelperForDesiredRole()` so it no longer re-specs bots to satisfy RDF demand. It now only rebuilds the bot in its current spec when talents/level are missing and returns failure immediately if the current spec cannot perform the requested role.

### Result
Queue role is now forced to derive from actual spec truth rather than the planner reshaping the helper to fit demand. This should eliminate cases like resto druids tanking or DPSing, fury warriors tanking, and enhancement shamans healing.

### Next proof targets
- Restoration druid must only ever surface as healer.
- Balance druid must only ever surface as DPS.
- Feral druid may surface as tank or DPS.
- Discipline priest may surface as healer or DPS.
- No queue helper should change spec solely because the planner wanted a different role.
