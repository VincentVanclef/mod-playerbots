# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8f
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore RTG planner-driven BG helper acquisition after 2.3.8e regression
Commit Description: Repairs the 2.3.8e battleground acquisition regression by making RTG BG helper dispatch consume planner-issued per-team need keys directly, preserving startup fills while also honoring live_refill and finish_fill as incremental unresolved demand.

--------------------------------

## Module Purpose

This revision repairs a hard runtime regression introduced during the mature-fill handoff work where RTG battleground demand could still be computed by the planner, but acquisition either produced no BG buckets or suppressed dispatch incorrectly. The fix re-aligns `RandomPlayerbotMgr` with the RTG planner contract instead of falling back to implicit absolute-size inference.

--------------------------------

## Architecture Overview

- `RtgBgQueuePlanner` remains the authoritative battleground demand producer.
- `RandomPlayerbotMgr` now treats `rtg_bg_team_need:<queue>:<bracket>:<team>` as the primary battleground helper acquisition signal.
- Legacy absolute team-size inference remains only as a fallback when explicit planner team-need is unavailable.
- `RtgQueueLifecycle` ownership, grace protection, and safe retirement rules remain unchanged.

--------------------------------

## Runtime Control Path

real player queues or active battleground matures
→ `RtgBgQueuePlanner` computes phase and per-team unresolved need
→ planner writes `rtg_bg_team_need:*` and `rtg_bg_need_total`
→ `RandomPlayerbotMgr::AddRandomBots()` builds BG acquisition buckets from planner keys
→ helper login begins with RTG BG add-data metadata
→ helper immediately joins queue through existing RTG dispatch path
→ queue/lifecycle ownership continues protecting helper until safe release conditions are met

--------------------------------

## Data Structures

Existing RTG battleground event keys remain authoritative:
- `rtg_bg_need_total`
- `rtg_bg_real_demand:<queue>:<bracket>`
- `rtg_bg_team_need:<queue>:<bracket>:<team>`
- `rtg_bg_phase:<queue>:<bracket>`

Existing helper state markers remain in force:
- `add`
- `rtg_bg_pending`
- `rtg_bg_queue_grace`
- `rtg_bg_retire_when_safe`

--------------------------------

## Config Interface

No new config keys in this revision.

Relevant existing settings:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.SmartQueue.Enable`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.MaxRandomBots`
- `AiPlayerbot.RandomBotAutologin`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

Still uses cached battleground sizing from `world.battleground_template` through the planner path. No new schema or SQL changes.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Bot/RtgBgQueuePlanner.cpp`
- `src/Bot/RtgQueueLifecycle.cpp`
- RTG battleground event cache

--------------------------------

## Lifecycle Model

- Planner demand remains separate from lifecycle ownership.
- Acquisition now fulfills planner-issued incremental team need directly.
- Already online RTG helpers only suppress the specific planner bucket they are already assigned to.
- BG helpers still cannot retire while queue, invite, battleground, or grace ownership says they are unsafe to release.

--------------------------------

## Known Constraints

- This revision is focused on battleground acquisition correctness after the 2.3.8e regression.
- It does not alter the standalone RTG lifecycle doctrine or revert any ownership protection work.
- Runtime validation is still required on live-like queue flow to confirm no branch-specific queue API quirks remain.

--------------------------------

## Future Evolution Hooks

- planner-consumed bucket diagnostics showing planner-need vs assigned-extra per team
- explicit queue-dispatch retry breadcrumbs for mature-phase helpers
- optional fairness shaping across simultaneous BG queue families

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Restores battleground helper logins after the 2.3.8e regression.
- Startup BG helper acquisition once again works from real queued demand.
- `live_refill` and `finish_fill` continue to ramp from planner-issued incremental team need.
- BG dispatch no longer requires local absolute team-size inference to rediscover mature-fill demand.
- Fixes a scoping bug in BG login metadata setup so `rtg_bg_queue_grace` is only applied to BG helpers.

--------------------------------

## Test Plan

1. Set `AiPlayerbot.RandomBotAutologin = 0`.
2. Set `AiPlayerbot.MaxRandomBots` high enough for full BG ramp testing.
3. Enable RTG event-driven queue assistance and RTG queue ownership.
4. Restart `worldserver`.
5. Queue one real level-19 player into Eye of the Storm.
6. Confirm startup helper login resumes and seeds to `MinPlayersPerTeam` from `world.battleground_template`.
7. Watch logs for:
   - `[RTG][BG][PHASE]`
   - `[RTG][BG][DEMAND]`
   - `[RTG][ACQUIRE][HEADROOM]`
   - `[RTG][BG][ACQUIRE]`
   - `[RTG][QUEUE][DISPATCH]`
8. Let the battleground mature past 90/180/270/360 seconds and confirm additional helpers log in at each planner ramp step.
9. Remove players from an active BG and confirm refill still occurs.
10. Let real demand disappear and confirm helpers only retire when queue/BG/lifecycle conditions say release is safe.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes a key RTG queue-assistance rule: battleground acquisition must consume the planner’s explicit per-team unresolved need, not attempt to reconstruct mature-fill intent indirectly from already-online helper totals. The planner is the source of truth; acquisition is the fulfiller.

--------------------------------

## 2026-03-19 — Arena orphan-demand collapse and cleanup-side team validation

## Problem Surface
Arena helper bots could remain online after all real arena demand disappeared. This was showing up as residual helpers after skirmish testing even though no player remained queued. At the same time, cleanup-side mismatch handling had an alliance-side blind spot because desired team `0` was not being treated as a valid assigned team.

## Root Cause
Two issues compounded:
- arena scaffolding still trusted raw arena queue activity too much when deciding whether demand was alive
- helper cleanup used `desiredTeam && actual != desired`, so assigned side `0` never triggered mismatch logic

## Repair
- arena scaffolding now treats demand as real-player-owned pressure (`realPlayers > 0 || activeInstances > 0`)
- arena demand now sizes through `matchesNeeded`, `matchSize`, and `targetTotal`
- explicit orphan breadcrumbs were added for bot-only arena residue
- cleanup-side owner breadcrumbs were added so lingering helper state can be diagnosed directly from logs
- desired team `0` now participates in wrong-team validation exactly like desired team `1`

## Files Modified In This Revision
- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

## Expected Runtime Benefit
- fewer arena helpers lingering after tests
- better visibility into arena residue vs real demand
- safer side-validation during arena cleanup

## 2026-03-20 — Arena shell planner phase 2: shell-side vs faction-side normalization

### Intake reason
Queue work had stabilized ownership/orphan retirement enough to expose the next deeper arena issue: arena helper planning was still conflating queue shell side with actual character faction.

### Why that matters
That conflation weakens several RTG goals at once:
- multi-arena concurrency
- same-faction skirmish support
- correct helper-side compatibility for the real queued player
- evidence clarity when diagnosing why a specific arena failed to pop

### What changed
- arena helper planner buckets now carry both a **logical shell side** and a **preferred actual faction**
- arena helper login metadata now preserves that preferred faction in the arena add-data payload
- arena cleanup and login-side mismatch checks now validate against the preferred faction instead of treating shell side as faction truth
- new `[RTG][ARENA][PLAN]` logs expose shell-by-shell planning evidence
- arena acquire logs now show `shellSide` plus `preferredFaction`

### Resulting doctrine update
Arena should be reasoned about as a **two-layer lane**:
1. shell construction decides which side of the match still needs population
2. faction compatibility decides what actual character may safely satisfy that shell

Do not collapse those back into one field in future passes.

## 2026-03-20 — Phase 3 follow-up: event-driven helper account sizing and RDF starvation recovery

### Runtime evidence
Live RTG queue logs showed:
- EOTS could reach 7v7 shell fill
- other BG lanes remained underfilled or repeatedly stalled
- RDF demand failed to progress while BG demand was active
- helper-account shortage errors appeared despite a large configured `RandomBotAccountCount`

### Root cause summary
Two queue-system assumptions were too narrow:
1. event-driven helper account assignment still used chars-per-account math inherited from legacy random-bot provisioning
2. queued RDF buckets were skipped whenever any BG demand existed, which starved RDF in mixed queue scenarios

### Recovery actions recorded in this chronicle
- changed RTG event-driven helper account sizing to one-account-per-helper-ceiling
- changed RTG RNDbot assignment sizing to the same one-account-per-helper rule in event-driven mode
- changed event-driven missing-account diagnostics to use account divisor `1`
- removed the acquire-side guard that blocked non-active-dungeon RDF buckets simply because BG demand existed elsewhere

### Engineering lesson
RTG queue-helper mode is a concurrency system, not a static character inventory system. Any sizing or admission rule that assumes one account can satisfy multiple simultaneous helpers will under-provision the queue lanes and create fake planner failures.

## 2026-03-21 — Phase 3 RDF planner authority handoff

### What changed
The RDF planner extraction is now advanced from “helper file exists” to “manager calls planner.”

`RandomPlayerbotMgr::CheckLfgQueue()` now:
- observes real RDF/LFG demand
- builds `RTG::RtgLfgQueueOwnerSnapshot`
- delegates event publication to `RTG::RtgRdfQueuePlanner`

The shared role/spec truth is also now routed through `RtgRdfRoleResolver` from the manager side instead of preserving a second local implementation.

### Why this matters
This removes the split-brain failure mode where future work could silently patch the planner file while runtime behavior continued to come from old inline manager code.

### Scope discipline
This pass does **not** claim Phase 4/5 completion. It is specifically the Phase 3 authority handoff required by the extraction checklist.

## 2026-03-21 — Phase 3 hookup completion: arena planner breadcrumbs + pending-ledger cleanup
- Added arena planner publication into `RtgBgQueuePlanner.cpp` so queue families that are arena-backed now emit RTG phase/demand breadcrumbs instead of living mostly outside the planner surface.
- Corrected pending-helper pressure accounting so `WorldIdle` ledger rows no longer count as active pending admissions.
- Corrected busy-account reconstruction so stale non-retired ledger rows do not keep blocking fresh helper acquisition.
- Ownership audit now removes stale offline helper ledger rows when there is no surviving `add` or `logout` truth behind them.

## 2026-03-21 — Phase 3 validation + Phase 4/5 surgical pass: RDF dispatch materialization, multi-queue phase correction, and arena pop breadcrumbs

### Runtime evidence that triggered this pass
Fresh RTG queue logs showed a narrow but decisive pattern:
- RDF helpers were being acquired and logged into `add`, but runtime never progressed beyond acquire/dispatch-add breadcrumbs
- battleground demand could exist across multiple queues, yet one lane monopolized effective progress while others remained planned-but-unmaterialized
- arena queues had planner breadcrumbs after the prior hookup pass, but there was still not enough runtime proof of helper formation and actual pop attempts

### Root causes confirmed in code
This pass verified three concrete defects instead of guessing:

1. **RDF had no manager-owned immediate join/accept dispatch surface**
   - BG helpers already had `RTG_DispatchImmediateBgQueueJoin(...)`
   - RDF helpers relied on eventual AI strategy cadence only
   - result: helpers could log in, retain pending markers, and still never emit a clear queue-join/accept progression surface during the narrow event-driven window

2. **BG multi-queue prioritization still had phase-number drift**
   - planner phase values are authoritative as:
     - `1 = starter_fill`
     - `2 = pop_or_invite`
     - `3 = live_refill`
     - `4 = finish_fill`
   - acquire-side priority/suppression logic was still treating old numbers/comments as truth
   - result: queue ordering and finish-fill suppression could target the wrong buckets, which helps explain “only one queue seems to fully work” under overlapping demand

3. **Shared-surplus lane arbitration still skipped queued RDF buckets whenever BG demand existed**
   - that guard contradicted the intended Phase 4 multi-lane fairness doctrine
   - result: RDF could still be starved in shared-capacity conditions even after earlier partial recovery work

### Manual corrections applied
- `RandomPlayerbotMgr.cpp`
  - added immediate RDF dispatch helpers for:
    - queue join
    - proposal accept
  - added manager-owned RDF runtime breadcrumbs:
    - `[RTG][RDF][JOIN]`
    - `[RTG][RDF][ACCEPT]`
    - `[RTG][RDF][FAIL]`
  - added generic success/failure dispatch breadcrumbs:
    - `[RTG][DISPATCH][SUCCESS]`
    - `[RTG][DISPATCH][FAIL_REASON]`
  - added explicit RDF helper login breadcrumb:
    - `[RTG][LFG][LOGIN]`
  - corrected BG phase priority mapping to match planner truth
  - corrected finish-fill suppression checks to target actual `phase == 4`
  - removed the remaining shared-surplus RDF starvation guard so queued RDF buckets can compete during multi-lane operation
  - added `[RTG][BG][MULTI_QUEUE]` capacity-sharing breadcrumb
  - added `[RTG][BG][ASSIGN]` helper-assignment breadcrumb
  - added `[RTG][ARENA][FORM]` breadcrumb when arena helper assignments materialize
- `BattleGroundJoinAction.cpp`
  - added `[RTG][ARENA][POP]` breadcrumbs around actual arena queue pop attempts
- `LfgActions.cpp`
  - added explicit RDF join/accept breadcrumbs inside the LFG action surface so runtime now proves whether the helper actually queued and accepted

### Behavioral changes after this pass
- RDF helpers should no longer stop at acquire/add visibility only; the manager now actively pushes them into join and accept attempts and logs the result
- overlapping queue families should no longer suffer phase-priority drift between planner and acquire logic
- finish-fill suppression is now aimed at real finish-fill buckets rather than the wrong phase ID
- RDF should retain a fair chance to use shared helper surplus even when BG demand exists simultaneously
- arena queues now expose helper formation and pop-attempt breadcrumbs, making “planner exists but no pop proof” far easier to diagnose

### Assumptions corrected
- The missing Phase 3 proof surface was **not** just “add more RDF logs” — the real gap was that BG had manager-owned immediate dispatch while RDF did not.
- The multi-queue bug was **not** solely a planner failure — acquire-side phase-number drift was still warping bucket priority/suppression behavior.
- Earlier partial RDF-starvation recovery was **not yet complete** — one shared-surplus guard still skipped queued RDF buckets under BG pressure.


## 2026-03-21 — RDF audit follow-up: silent helper login failure surfaces and cooldown quarantine

### Runtime evidence
Fresh RDF testing improved from total join failure to partial helper materialization:
- tank/healer/one DPS could now log in and reach RDF join dispatch
- extra requested DPS helpers often never reached `[RTG][DISPATCH][ADD]` or `[RTG][LFG][LOGIN]`
- after ~30–35 seconds they decayed through `[RTG][DISPATCH][STALL]` and the system misreported the symptom as generic account shortage

### Confirmed cause
The queue system still had a blind spot on the **login materialization** edge:
- several `AddPlayerBot(...)` failure exits in `PlayerbotMgr.cpp` returned silently for rndbot queue helpers
- the null-player callback path also returned without notifying RTG helper cleanup
- those helpers therefore sat with live `add` / pending markers until the dispatch stall watchdog cleaned them much later

This meant a character-load failure could masquerade as queue starvation or account exhaustion.

### Repairs applied
- queue-managed rndbot login failures now call `RandomPlayerbotMgr::OnPlayerLoginError(..., reason)` on the previously silent exits:
  - `bot_loading_guard`
  - `already_in_world`
  - `missing_account`
  - `holder_initialize_failed`
  - `session_player_null`
- `OnPlayerLoginError` now:
  - clears `rtg_add_requested` immediately
  - records `[RTG][LOGIN][FAIL] ... reason=...`
  - quarantines the failed helper with short `rtg_login_fail_recent` cooldown so reacquire does not hammer the same broken character repeatedly
- successful RDF helper login now clears `rtg_add_requested` just like BG helper login already did

### Doctrine correction
A dispatch stall is not always a queue-join problem. If helper login materialization has silent exits, Phase 3 can look partially healthy while Phase 5 is still broken at the login boundary. Queue-managed login failure must be explicit and immediate.


## 2026-03-21 — RDF completion follow-up: login materialization failures must clear ownership, and planner role intent must survive join

### Runtime evidence
Live RDF tests still showed two distinct failure families even after the earlier Phase 3 hookup work:
- some helpers reached acquire but never reached login, then sat until `[RTG][DISPATCH][STALL]`
- some logged-in helpers hit RDF join dispatch but failed before entering LFG state, especially healers

### Root causes confirmed
1. **Queue-managed helper login still had silent early exits inside `PlayerbotMgr.cpp`**
   - missing account / query-holder initialize failure / null player session load could all return without telling RTG queue ownership that the helper never materialized
   - result: helper reservations remained live until the later stall watchdog, inflating pressure and slowing replacement

2. **RDF join still treated runtime spec detection as stronger than planner role intent**
   - `GetRoles()` always returned `actualRole` even for assigned RTG helpers
   - `JoinLFG()` could logout an assigned helper merely because spec inference differed from the planner reservation
   - result: valid class-capable helpers could be discarded before ever sending `CMSG_LFG_JOIN`, violating the queue-architecture rule that RDF role intent must survive acquisition through dispatch

### Manual corrections applied
- `PlayerbotMgr.cpp`
  - added queue-managed login failure routing to `RandomPlayerbotMgr::OnPlayerLoginError(...)` for:
    - `missing_account`
    - `holder_initialize_failed`
    - `session_player_null`
- `LfgActions.cpp`
  - assigned RTG RDF helpers now use **planner desired role** as the join role when the class can legitimately perform that role
  - class-incompatible reservations still fail closed and retire
  - removed the old spec-mismatch auto-logout path for class-compatible assigned helpers

### Doctrine correction
For assigned RTG RDF helpers, **planner role intent is the queue authority**. Runtime spec detection is diagnostic and can influence AI behavior later, but it must not preempt queue admission when the class is role-compatible and the reservation was deliberately made for that role.


## 2026-03-22 — RDF role-faithfulness correction

### Runtime evidence
Live RDF testing proved groups could now form quickly, but role correctness regressed: helpers were entering RDF under planner-requested roles that did not match their loaded runtime spec, producing cases like restoration druids tanking and retribution paladins healing.

### Wrong assumption corrected
Planner authority is not permission to override runtime specialization truth. Planner role intent must survive acquisition and dispatch, but only through helpers whose runtime spec actually satisfies that intent.

### Repair
- removed the effective planner-overrides-runtime behavior from `LfgJoinAction::GetRoles()`
- added runtime role mismatch fail-closed handling before RDF join dispatch
- added manager-side runtime role mismatch retirement so wrong-spec helpers are logged out and reacquired instead of entering RDF under false role masks

### Doctrine update
RDF role correctness is a two-step contract:
1. planner chooses the missing role
2. runtime helper validation must confirm the logged-in bot actually matches that role before queue admission

If those disagree, reacquire. Do not force the queue packet to pretend the helper is a different spec.

---

## 2026-03-22 — RDF proposal acceptance packet order correction

### Symptom
- RDF helpers joined quickly and role composition improved.
- Proposal ready-checks popped, but queued helpers did not reliably accept.
- Live symptom on player side: proposal kept expiring or being declined, then re-popping with the same helper set.
- RTG runtime logs showed repeated `[RTG][RDF][JOIN]` success breadcrumbs, but no matching `[RTG][RDF][ACCEPT]` breadcrumbs for the affected helpers.

### Confirmed cause
The accept path in `LfgAcceptAction::Execute` was parsing `SMSG_LFG_PROPOSAL_UPDATE` in the wrong field order. The code read the packet as:
- `dungeonId`
- `state`
- `proposalId`

For 3.3.5a acceptance purposes, the proposal id is the leading `uint32`. That meant the packet-triggered accept path often failed to recover a valid proposal id, so the queued helper never sent `CMSG_LFG_PROPOSAL_RESULT` with `accept=true`.

### Doctrine correction
Planner/manager/join can be healthy and still fail the dungeon form if proposal acceptance misreads packet authority. In RDF, proposal id extraction is a hard owner boundary between:
- queue materialization
- final group lock-in
- teleport into dungeon

### Fix applied
- Read the leading `uint32` from `SMSG_LFG_PROPOSAL_UPDATE` as the proposal id.
- Persist it immediately into the `lfg proposal` AI value.
- Keep packet-triggered immediate acceptance for queued RDF helpers.
- Add RTG debug breadcrumb for proposal packet receipt so future regressions can be isolated faster.

### Expected proof after fix
A healthy RDF completion should now show:
- `[RTG][RDF][JOIN]`
- `[RTG][RDF][ACCEPT]`
- proposal lock-in
- dungeon teleport / run

instead of repeated proposal expiry loops with no accept breadcrumbs.

## 2026-03-22 — RDF deep audit: candidate-role targeting, login-failure surfacing, and per-role mismatch quarantine

### Runtime evidence
Recent live RDF tests proved the lane had advanced past acquire-only failure, but still lagged behind the BG lane in three key ways:
- helper selection still trusted offline talent reads too much, producing repeated runtime role mismatches
- some queued helper login failures were still silent at the `PlayerbotMgr` boundary
- repeated bad RDF candidates could burn through busy accounts and make one damaged queue look like global account starvation

### Architecture correction
RDF should mirror BG queue doctrine as closely as possible:
- planner owns demand
- acquire fulfills demand
- runtime validation confirms the selected helper really matches the requested role
- bad candidates must be quarantined quickly so they do not poison shared queue capacity

### What changed
- queued helper login failures now report through `OnPlayerLoginError(...)` for `bot_loading_guard`, `missing_account`, `holder_initialize_failed`, and `session_player_null` paths in `PlayerbotMgr.cpp`
- RDF now keeps a runtime role cache (`rtg_runtime_lfg_role`) learned from real helper logins/mismatches
- RDF mismatch now also places a per-role cooldown (`rtg_lfg_role_block:<role>`) on the mismatched helper so the same bot is not immediately re-selected for the same wrong role
- event-driven RDF acquisition now prefers known runtime role truth first, then falls back to offline role inference only when runtime evidence does not yet exist

### Why this matters
This moves RDF closer to the BG lane's stronger behavior: once a helper proves what it really is at runtime, the queue system should use that truth on later passes instead of repeatedly rediscovering the same mismatch through expensive login attempts.

### Expected next-proof signals
- fewer repeated `runtime_role_mismatch` failures for the same helper
- fewer false `RandomBotAccountCount` shortage signals during active RDF demand
- cleaner mixed-lane behavior when BG, RDF, and arena demand coexist

## 2026-03-22 — RDF audit continuation: proposal-id capture at packet ingress, stale role-cache precedence correction, and queue-helper gear hygiene

### Runtime evidence
Latest live RDF tests showed three intertwined symptoms:
- helpers were joining RDF quickly, but some proposals still expired with no matching `[RTG][RDF][ACCEPT]` proof on the bot side
- candidate selection could still treat a visibly role-correct helper (for example a holy priest) as the wrong offline target because stale runtime cache won over persistent talent truth
- queue helpers that churned across levels/spec refreshes accumulated bag overflow and mail spill, causing repeated `Player::_LoadInventory ... Item will be sent by mail` warnings and making later initialization less deterministic

### Confirmed architecture corrections
1. **Proposal acceptance needed a harder owner boundary than AI strategy timing**
   - relying only on the world-packet trigger path left a gap where proposal id might not be cached early enough for the manager-side accept dispatch
   - fix: cache `SMSG_LFG_PROPOSAL_UPDATE` proposal id immediately inside `PlayerbotAI::HandleBotOutgoingPacket(...)`, before normal trigger handling, so manager polling can always see the active proposal id

2. **Offline RDF selection must not let stale runtime cache outrank real offline talent truth**
   - once explicit talent preparation started persisting role-correct specs, the selector still prioritized `rtg_runtime_lfg_role` first
   - that could preserve old role memory after a later spec correction and make selection look wrong from the player's point of view
   - fix: prefer real offline talent truth first; only fall back to runtime cache when offline talent data is not available

3. **Queue-helper gear initialization needs hygiene, not infinite accumulation**
   - repeated queue-helper level/spec preparation was reinitializing gear while old backpack contents and overflow mail remained
   - fix: for queue-managed RDF helpers that actually need bootstrap work (missing talents / role mismatch / level mismatch), clear bag contents and purge mailbox records before fresh equipment initialization

### Doctrine update
RDF should mirror BG discipline not only at acquire/dispatch, but also at the **materialization hygiene** layer:
- capture proposal state at the earliest reliable packet ingress
- prefer persistent role truth over stale cache memory
- treat queue-helper bootstrap inventory as disposable state that must be cleaned before re-gearing

## 2026-03-22 — RDF proposal lifecycle lock pass: accept-sent state, join suppression during active proposal, and lock release only on dungeon entry or resolved failure

### Runtime evidence
Live RDF testing finally isolated the blocker beyond join/materialization:
- helpers logged in cleanly
- helpers joined RDF cleanly
- helpers emitted repeated `[RTG][RDF][ACCEPT]` and `rdf_accept` success breadcrumbs for the same proposal id
- yet the dungeon never finalized, and helpers fell back into repeated RDF join work while the ready-check window was still unresolved

This proved the helpers were no longer failing to *press accept*; instead, the queue system was failing to **hold them inside proposal resolution** as a first-class lifecycle phase.

### Confirmed root cause
`LfgAcceptAction` was still clearing the in-memory proposal state too early and resetting AI immediately after sending `CMSG_LFG_PROPOSAL_RESULT`. That let the event-driven RDF manager see the same helper as ordinary `LFG_STATE_NONE` / join-eligible work again before the dungeon transfer or failure boundary had resolved.

In practice, the RDF lane was doing:
- accept proposal
- forget proposal exists
- drift back into ordinary join dispatch
- accept again on the same unresolved proposal

That is architecturally weaker than the BG lane, where the pop/match transition is treated as an owned state and not re-entered as raw queue work.

### Doctrine correction
RDF needs an explicit **proposal lock** phase between:
- queue join
- proposal acceptance sent
- dungeon entry / proposal failure resolution

Once a queued helper has accepted a proposal, it must not:
- re-join RDF
- re-bootstrap role prep
- get treated as idle/ordinary LFG state again

until the proposal resolves by either:
- entering the dungeon, or
- timing out / failing and being intentionally released back to the queue lifecycle

### Fix applied
- Added queue-helper event-backed proposal lifecycle state:
  - `rtg_lfg_proposal_lock`
  - `rtg_lfg_accept_sent`
- Queued RDF helpers now mark proposal lock + accept-sent timestamp when `lfg accept` succeeds.
- Queued RDF accept no longer clears `lfg proposal` or resets AI immediately after sending accept.
- Event-driven RDF dispatch now suppresses all new join work while proposal lock / accept-sent state is active.
- Proposal lifecycle state now clears only when:
  - helper is confirmed in dungeon/LFG run state, or
  - the proposal never resolves after accept and is intentionally released as `proposal_not_resolved`
- Queue-state cleanup and login-error cleanup now also clear proposal lifecycle state so dead helpers do not retain stale proposal locks.

### Expected proof after fix
Healthy RDF completion should now look like:
- `[RTG][RDF][JOIN]`
- `[RTG][RDF][ACCEPT]`
- no repeated `JOIN` spam for that helper during the same proposal window
- dungeon entry / active run state

If a proposal genuinely fails, the expected breadcrumb becomes a single controlled release such as:
- `[RTG][RDF][FAIL] ... reason=proposal_not_resolved ...`

rather than endless accept/join oscillation.


## 2026-03-22 — RDF teleport phase audit (accept vs enter dungeon)

### Symptom
Bots were now reaching `JOIN` and `ACCEPT`, but still idled at the final **Enter Dungeon** stage. Live logs showed repeated `rdf_accept` success for the same proposal without any dungeon transfer.

### Root cause
The queue lane had advanced past proposal acceptance, but it still had no explicit **teleport phase**. In WotLK RDF flow, accepting the proposal is not the final client action; the client must still send `CMSG_LFG_TELEPORT` to actually enter the dungeon. RTG RDF helpers were latching proposal state, but once the group became an LFG group / dungeon state, there was no event-driven dispatch step to press the final teleport button.

### Doctrine correction
RDF lifecycle is not just `join -> accept`. It is `join -> accept -> teleport -> dungeon ownership`. BG already had a stronger pop/entry boundary; RDF needed the same explicit finalization phase.

### Fix direction
- Added `rdf_teleport` dispatch helper using the existing `lfg teleport` action.
- Added `rtg_lfg_teleport_sent` lifecycle state.
- When helper is in LFG group / dungeon state but not yet on a dungeon map, manager now dispatches the teleport action instead of rejoining or idling.
- Proposal lifecycle state is released only once helper is actually on a dungeon map.


--------------------------------

## RDF Proposal-Latched Teleport Finalization Pass

### Symptom Confirmed

- Queue helpers were reaching `JOIN` and `ACCEPT` reliably, but never actually entering the dungeon.
- Live logs showed repeated `RDF][ACCEPT` with no matching stable teleport phase completion.
- The prior teleport gate was still too strict because it waited for `group->isLFGGroup()` / `LFG_STATE_DUNGEON` evidence before sending the teleport packet, which can be later than the ready-dialog window the client exposes.

### Root Cause Correction

RDF finalization is not just `join -> accept -> wait`.
It is `join -> accept -> proposal-latched teleport enter request -> dungeon map transition`.

The queue system was already latching proposal ownership correctly, but it was not using that latched state strongly enough to drive the final `Enter Dungeon` packet.

### Fix Applied

- Manager-side RDF dispatch now treats proposal-latched state itself as sufficient to begin the final teleport phase.
- Teleport attempts are allowed once proposal acceptance is latched, even before `group->isLFGGroup()` becomes visibly true.
- Teleport retries are throttled on a short cadence instead of waiting on a narrower readiness proof that could arrive too late.
- `LfgTeleportAction` was hardened to send an explicit `uint8(0)` enter-dungeon payload for queued RDF helpers and to breadcrumb that phase directly.

### Expected Behavioral Change

A healthy RDF sequence should now look more like:

- `JOIN`
- `ACCEPT`
- `TELEPORT`
- dungeon map entry

without sitting indefinitely in a post-accept, pre-enter limbo.
