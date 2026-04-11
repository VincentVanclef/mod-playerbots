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


## 2026-03-24 — RDF acquire/dispatch same-cycle materialization correction

### Problem surface
RDF helper acquisition could emit the correct number of `[RTG][ACQUIRE][REQUEST]` lines, but only a smaller subset would receive `[RTG][DISPATCH][ADD]` and login on that same pass. The remainder would sit as pending add-data and later age into dispatch stall, creating the false appearance that the last helper simply "would not log in."

### Root cause
`availableBots` was snapshotted from `currentBots` before the event-driven RDF acquire pass. Newly acquired helpers updated `add` state and queue metadata, but were not enrolled into the live `availableBots`/`currentBots` working set for the same update cycle. That left dispatch operating on a stale pre-acquire view.

### Repair
- newly acquired RTG queue helpers are now appended immediately to `currentBots` if absent
- newly acquired RTG queue helpers are now appended immediately to the live `availableBots` working set if absent
- `rtg_target` is raised to at least `currentBots.size()` in event-driven mode so the same-cycle working set is not silently clipped by a stale target from the previous pass

### Expected runtime benefit
- RDF helper requests and dispatch/login now stay in the same control cycle much more reliably
- fewer false `DISPATCH][STALL]` cases for the last helper in a fresh RDF lane
- less misleading follow-on acquisition pressure caused by stale pre-acquire dispatch views

## 2026-03-28 — RDF proposal loop containment and one-shot acceptance guard

### Symptom that forced this pass
A previous RDF proposal fix created a live repeat loop: entering the final Dungeon Finder stage caused the same proposal path to fire repeatedly, destabilizing worldserver and starving the colocated website stack.

### Corrected owner seam
- packet hook observes proposal state only
- `LfgAcceptAction` owns the single guarded accept send
- RTG manager waits while proposal is unresolved instead of guessing finalization

### Manual changes
- removed direct accept send from `PlayerbotAI` packet hook
- parse real `SMSG_LFG_PROPOSAL_UPDATE` field order before acting
- accept only while proposal state is `LFG_PROPOSAL_INITIATING`
- added `rtg_lfg_accept_proposal` one-shot guard so helpers do not re-accept the same proposal id forever
- helper cleanup/login-error cleanup now clears this guard too

### Proof to demand next
- exactly one accept per helper per proposal id
- repeated proposal update packets only produce suppression/debug breadcrumbs
- no packet flood, no host overload, no premature replacement acquisition

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

### Proof signals required next
- when a feral druid helper is chosen for RDF, logs should show capability breadcrumbs containing `actualMask=10` (`TANK|DPS`) rather than treating feral as tank-only
- a known offline feral druid should now be selectable when RTG needs either a tank helper or a DPS helper
- a feral helper already in a valid feral spec should **not** be forcibly respecced to balance merely because the current request is DPS
- join logs for the feral helper should show `roleMask=10` and role text `TANK/DPS`
- no regression in healer-only specs or strict single-role specs

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

### Proof signals required next
- no helpers should silently leave or lose party simply because they are in an LFG group outside the map for a short transition window
- after `rdf_teleport reason=group_ready`, the same helper should **not** later fall back into `proposal_not_resolved` for that same advanced transition
- stale old-dungeon helpers should log `[RTG][RDF][RECOVER] ... reason=stale_instance` instead of waiting forever unavailable inside an old instance map
- dead helpers should log `[RTG][RDF][RECOVER] ... reason=dead_before_join` and stop spamming repeated `join_dispatch_failed ... dead=1`
- proposal acceptance should occur faster, with manager-side success breadcrumbs like `lane=rdf_accept reason=proposal_visible`, but still no packet storm

### Chapter 2026-03-28 — RDF orphan grace + priest main-tank shielding pass

#### Summary
The RDF entry pipeline is now materially healthier: the group formed quickly, accepted the ready dialog cleanly, and no helper left the party on entry. The next seam exposed by live play was post-run orphaning. When the real player left the dungeon early, helpers stayed online under normalized `owner=1` ownership and repeatedly attempted to rejoin RDF even though they were now just stragglers from a finished/abandoned run. Priest healing also still lagged behind tank damage intake.

#### Resolution
- Added a bounded `rtg_lfg_orphan_since` lifecycle marker and a 180-second orphan grace in `RandomPlayerbotMgr.cpp`.
- Helpers that no longer have live real-player demand, are not grouped, and are no longer in the dungeon map now stop rejoining RDF and instead age toward a clean logout.
- Stale queue sweeps ignore already-orphaned helpers to prevent repeated join/stale churn.
- Priest shielding now explicitly prefers `main tank`, and heal-priest priorities were made more proactive so tank response begins earlier.

#### Files touched
- `mod-playerbots/src/Bot/RandomPlayerbotMgr.cpp`
- `mod-playerbots/src/Ai/Class/Priest/Action/PriestActions.cpp`
- `mod-playerbots/src/Ai/Class/Priest/Strategy/HealPriestStrategy.cpp`

## Chapter 2026-03-28E — Stable RDF owner identity and assigned-role join discipline

### Situation
Post-hardening RDF tests demonstrated healthy proposal acceptance and dungeon arrival, but later tests surfaced false owner identities (`1`, `2`) after LFG group materialization and refill instability around flex-role helpers.

### Findings
- The previous owner normalization path and real-player bucket discovery were still allowing LFG group GUID identity to leak into RTG ownership/accounting.
- Flex-role join masks were useful for capability reasoning but too broad for already-assigned refill slots.
- Discipline priest capability needed to be widened for RTG helper matching.

### Changes
- Replaced synthetic group-owner normalization with stable real-player owner selection.
- Used stable real-player owner identity in real-player RDF demand bucket construction.
- Kept flex capability for acquisition/prep but enforced assigned-role queue join discipline.
- Treated Discipline as safe healer/dps flex for resolver capability.
- Raised priest main-tank shielding pressure and corrected summon landing Z.

### Proof targets
- No more helper add-data / dispatcher churn under owner ids `1` or `2` for solo RDF tests.
- Second initiating character gets clean helper acquisition.
- Refill works without feral dual-mask blocking the remaining missing slot math.

## Chapter 2026-03-30-C — RDF owner drift pinning + strict paladin queue doctrine
- Scope: mod-playerbots / RTG queue assistance / RDF helper lifecycle.
- Findings:
  - Live RDF success exposed a later regression path where helper work drifted onto low synthetic owners (`1`, `2`) during follow-up/refill flows.
  - Holy paladin tanking is an unacceptable class-role breach and must be treated as a hard runtime mismatch, not a soft mismatch.
- Delivery:
  - queue helper ownership remains pinned to the initiating real player.
  - paladins now require exact assigned-role/runtime-role agreement before RDF dispatch proceeds.
  - priest shielding pressure was raised again so support feels more proactive.
- Proof targets:
  - no more helper acquisition or dispatch keyed to owner `1` / `2` unless that is the actual real player.
  - holy paladin never survives a tank dispatch path.
  - priest shield uptime improves on tank / supported player targets.

## Chapter 2026-03-30 — Multi-owner dispatch floor
- Situation: later owners/lane requests were stalling behind the first owner even though helper acquire requests were being created.
- Root seam: login budget relied on a planner snapshot that could lag behind already-requested queue-managed helpers.
- Correction: the RTG target now respects a managed helper floor so already-requested queue helpers preserve enough login headroom to connect.
- Watchpoints: verify concurrent RDF owners plus BG demand can all emit DISPATCH ADD / LOGIN without synthetic account starvation.

## Chapter — Pending login floor for simultaneous RDF lanes
- Symptom: first two simultaneous RDF runs filled; third run only produced helper requests and later `DISPATCH][STALL]`, with false account-capacity messages.
- Root seam: requested helpers in `currentBots`/add-data were not guaranteed enough login budget while earlier owner groups stayed online.
- Fix: during RTG event-driven login budgeting, preserve `online + pendingQueuedLogins` as the minimum live target so requested helpers can actually connect.


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
- Symptom: active RDF runs continued to own the shared helper lane, blocking new owners from opening later RDF fills unless all queued together.
- Root seam: planner demand still counted active dungeon owners as open RDF helper requests.
- Correction: close RDF request ownership once the run is filled/materialized, while leaving active helpers online for the dungeon itself.
- Next proof: queue owner A, let the run fill and enter, then queue owner B afterward and confirm B reaches ACQUIRE -> ADD -> LOGIN without waiting for A to end.


## Chapter 2026-04-03 — Multi-owner queue lane login starvation
**Context**
Sequential owners were still stalling even after previous planner/lane-closure passes.

**Finding**
The missing seam was not only owner demand accounting. Already-acquired queue helpers were sitting in `currentBots` with add-data, but the live login pass only iterated `availableBots`. That let the first owner get serviced while later owners aged into dispatch stalls.

**Change**
Updated the login phase in `RandomPlayerbotMgr.cpp` to process offline queue-managed `currentBots` before the normal `availableBots` pass.

**Why this matters**
This makes RDF, BG, and arena helper service truly owner-driven instead of timing-window-driven.

--------------------------------

## Chapter — RDF Assigned-Helper Lane Closure Correction

### Context
A live Owner A test showed a deeper RDF request-lane error before multi-owner overlap could even be trusted. Helpers were acquired, added, and logged in successfully, but then repeatedly failed `join_dispatch_failed` while staying in `LFG_STATE_NONE`.

### Incorrect Assumption
The planner had treated `helperAssigned*` counts as sufficient to close the owner's request lane. That was too early. Assigned helpers are not yet queued/live helpers; they are merely owed queue-join work.

### Real Owner Seam
RDF planning needs two different views of role coverage:
- **acquisition coverage** for preventing over-requesting new helpers
- **lane coverage** for deciding whether the owner's active RDF request is still open

Closing the lane from assigned-helper counts alone caused `rtg_lfg_real_demand` to drop before the assigned helpers finished joining, which then blocked the join path inside `LfgJoinAction`.

### What Changed
`RtgRdfQueuePlanner` now computes:
- `acquireNeed*` from real + helperQueued + helperAssigned
- `laneNeed*` from real + helperQueued only

The owner's RDF lane remains open until queued/live coverage is complete and no offline assigned helpers remain outstanding, or until the run is already active in-dungeon.

### Proof To Look For
- Owner A helpers should progress from login into actual RDF join instead of repeated `join_dispatch_failed`
- `rtg_lfg_real_demand` should remain live while assigned helpers are still offline/not queued
- helper acquisition should still avoid duplication because `acquireNeed*` continues subtracting assigned helpers


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


## 2026-04-03 — Queue helper return-to-world retirement tightening
- Finding: helpers were correctly serving RDF/BG/arena lanes, but after returning to the world they could linger online because retirement logic still applied long grace windows or generic ownership delays even after the lifecycle had ended.
- Change: RDF orphan retirement after a real dungeon run returning to the world now retires in about 5 seconds instead of lingering for minutes. BG/arena helpers marked `rtg_bg_retire_when_safe` now retire immediately once they are back in the world and no longer in queue/invite/BG state. Finished dungeon cleanup was also reduced from 300 seconds to 5 seconds after world return.
- Proof target: after an RDF/BG/arena completes and the player leaves the activity, helpers should log out almost immediately after returning to the world, while still avoiding disruption during initial login or active run stages.

## 2026-04-04 — Exact-role hybrid prep tightening and bot-only group return-to-world retirement
- Finding: live multi-owner testing improved RDF/BG/arena concurrency, but two seams remained. First, exact-role hybrids such as paladins could still enter wrong RDF roles when helper prep fell back to runtime spec identity without reliable offline spec truth. Second, detached RDF helpers often remained online after real players left because return-to-world retirement still treated bot-only LFG groups as if the run was still active.
- Change: `RTG_PrepareLfgHelperForDesiredRole()` now refuses RDF prep for hybrid classes (druid, paladin, priest, shaman, warrior, death knight) unless reliable offline spec truth exists, preventing exact-role hybrids from being rebuilt or admitted from ambiguous runtime fallback. Return-to-world RDF retirement now treats bot-only groups as detached, shortens `rtg_lfg_world_return_since` retirement to ~5 seconds, and finished-dungeon cleanup no longer treats a bot-only LFG group as an active run.
- Effect: Paladins and other exact-role hybrids must derive RDF role from real offline spec truth rather than ambiguous runtime fallback, while helpers that return to the world without any real player still attached retire quickly instead of lingering online and being reused.
- Proof target: Holy/Protection/Retribution paladins must queue only as healer/tank/dps respectively, and when a player leaves a dungeon early the remaining detached helpers should all retire shortly after world return even if they stay grouped among themselves.


## Chapter: Hybrid prep mismatch enforcement + bot-only world-return retirement
- Cause: `RTG_PrepareLfgHelperForDesiredRole()` could return false for hybrid spec mismatch or missing offline spec truth, but callers were not checking whether the failure meant "blocked" versus "no rebuild needed". This let exact-role hybrids still slip through queue prep.
- Correction: added an explicit `blocked` out-flag so login and dispatch can reject the helper immediately when prep proves the requested role is incompatible with real spec truth.
- Additional correction: detached RDF helpers that had returned to world could remain grouped only with other bots, which kept safe-retire evaluation too conservative. Before requesting logout, the code now removes the bot from a bot-only group and uses queue-helper logout with queue-state clearing.
- Proof targets: no Fury/Arms warrior can survive as RDF tank, no Holy/Ret paladin can survive as wrong role, and all detached helpers retire promptly after return-to-world when no real player remains.


## Chapter: Strict RDF spec prep correction and bot-only return-to-world retirement
- **Date:** 2026-04-04
- **Subsystem:** mod-playerbots / RTG queue service
- **Context:** Live testing showed two remaining seams: (1) hybrid helpers, especially druids, could still enter wrong RDF roles when runtime spec drifted from offline spec truth; (2) BG/RDF helpers returning to the world after the player left often stayed online because they remained in bot-only groups and safe-retire never completed.
- **Finding:** RDF prep was only rebuilding helpers when level or talents were missing. If offline spec truth disagreed with the helper's current runtime spec, prep could still accept the desired role without realigning the bot to the stored spec. Separately, retirement logic still treated bot-only groups as a reason to keep helpers online.
- **Change:** Added `specMismatch` handling inside `RTG_PrepareLfgHelperForDesiredRole()` so offline spec truth now forces a rebuild when runtime spec tab differs. Added `RTG_LeaveBotOnlyGroup()` and used it before RDF/BG queue-helper logout so returned-to-world helpers can detach and retire cleanly.
- **Expected proof:** No balance druid healer / resto druid tank leakage through runtime drift, and helpers returning to the world after RDF/BG completion or early player exit should actually log out instead of repeating `RETURN_WORLD`/orphan breadcrumbs while lingering online.

## Chapter: Queue role override narrowed to true flex specs; bot-only BG matches treated as orphaned
- **Date:** 2026-04-04
- **Subsystem:** mod-playerbots / RDF role identity / BG retirement

### Situation
A recent RDF test was mostly correct, but one protection warrior could still behave as DPS when slotted into a DPS position. Separately, after a real player AFK-left WSG, the battleground continued as a full bot match and helpers remained active even though no real player still needed the match.

### Root cause
1. `rtg_lfg_strategy_role` was still being honored by `PlayerbotAI::IsTank/IsHeal/IsDps` for all classes during queued/pending RDF states. That is only safe for true flex specs such as feral druids and discipline priests. Exact-role classes (warrior, paladin, shaman, etc.) should never have their identity overridden by queued role state.
2. BG planning still treated an active bot-only battleground as real demand if stale queue counts remained. Also, protected BG helpers were allowed to stay in a bot-only battleground for too long after the last real player left.

### Fix
- Narrowed queued-role override honoring in `PlayerbotAI` to only true flex specs:
  - feral druid (tank/dps)
  - discipline priest (heal/dps)
- Exact-role classes now keep their spec-derived identity even while RDF queue state exists.
- In `RtgBgQueuePlanner`, an active battleground with zero real active players is now treated as a bot-only orphaned match, clearing real demand even if stale queue counts linger.
- In `BattleGroundJoinAction`, protected helpers may now leave bot-only battlegrounds quickly once no real players remain.

### Expected proof
- Protection warriors should no longer act as DPS due to queued-role override.
- If the last real player leaves a BG, the match should stop being treated as real demand and helpers should begin leaving rather than sustaining a full bot-only game.

### 2026-04-04 - Arena/BG arbitration and live-demand cleanup
- Prioritized arena buckets over battleground refill buckets during shared RTG BG-lane acquisition so solo arena demand no longer loses repeatedly to battleground finish-fill.
- Planner now derives real BG/arena demand from currently connected real players' live queue slots instead of trusting stale BattlegroundInfo queue counters after players leave or AFK out.
- BG retirement now forces a bot-only battleground leave request once no real players remain, then clears queue helper state after detach.


## Chapter: Arena/BG Detach Finalization and Bot-Only BG Group Ownership Correction

**Context:** Arena helpers were still lingering in the world after arena completion when battleground demand remained active, and final battleground retire waves risked destabilizing the server.

**Findings:** The remaining coupling was not planner identity anymore but post-lifecycle ownership semantics. Arena/BG helpers could remain lifecycle-owned solely because a detached bot-only BG group still existed, even after they had already returned to the world. That blocked retirement. Separately, logging out every returned helper in one sweep increased risk during final cleanup waves.

**Changes:** `RTG_IsBgLifecycleOwned()` now ignores detached bot-only BG groups once the helper is no longer in queue/BG/arena/map lifecycle. BG/arena helpers now use a dedicated `rtg_bg_world_return_since` staging window before logout, and retirement is batched per tick rather than sweeping every helper at once.

**Expected proof:** Arena helpers should retire independently of an unrelated ongoing battleground. BG helpers should leave bot-only matches cleanly after the last real player is gone, without a final-wave server destabilization during logout cleanup.


## Chapter 2026-04-08 — Major lane isolation audit patch
- Re-audited queue ownership after arena helpers continued to surface as `rtg_bg:*:9` in runtime logs.
- Hardened arena detection in `RandomPlayerbotMgr.cpp` through a dedicated lane helper instead of relying only on raw `BGArenaType(...)` checks.
- Switched BG/arena helper acquisition to use lane-aware arena detection when building buckets and add-data.
- Split queue helper reservation state so arena helpers use `rtg_arena_pending` / `rtg_arena_queue_grace` instead of reusing BG pending markers.
- Tightened offline RDF role filtering to derive the mask from authored offline spec-tab role truth, reducing regressions such as holy-priest DPS or enhancement-healer assignment.
- Goal of this pass: make arena ownership lane-correct at acquire/assign time and reduce further cross-lane regressions before additional queue polish.

## 2026-04-08 — Hard lane isolation for arena + scoreboard-safe 30-second world-return retirement

### Problem surface proven by live logs
- arena queue `9` was still being acquired with BG add-data (`rtg_bg:*:9`) instead of true arena-native ownership
- arena helpers were still emitting `[RTG][BG][ASSIGN]` and `lane=bg queue=9`, proving lane contamination was happening at creation/assign time rather than only in log wording
- dormant arena states could still leave stale helpers online long enough to leak into later arena cycles
- BG/arena helpers were retiring too quickly after leaving battleground context instead of waiting a safe post-leave world-return window

### Root causes corrected
1. **Arena helper creation still used `MakeBgAddData(...)` inside the shared BG bucket acquire path.** That meant queue `9` and other arena lanes could be born as BG-owned from the first write.
2. **Arena planner state was still written through BG event keys in several places.** This allowed arena demand/phase/team-need truth to remain partially shared with BG surfaces.
3. **Lifecycle retirement checks still favored BG-only pending/grace keys in some shared ownership code.** Arena helpers could therefore survive or be evaluated against the wrong ownership surface.
4. **Post-leave retirement delay was only 5 seconds.** That was too short for the desired scoreboard/end-state behavior.

### Manual repair applied
- `src/Bot/RandomPlayerbotMgr.cpp`
  - changed queue-key helpers to become lane-aware so arena queues now resolve to:
    - `rtg_arena_need:*`
    - `rtg_arena_team_need:*`
    - `rtg_arena_phase:*`
  - changed the shared battleground/arena acquire path so arena buckets now create helpers with `RTG::MakeArenaAddData(...)` at birth instead of `RTG::MakeBgAddData(...)`
  - changed dispatch breadcrumbs so queue dispatch now logs `lane=arena` for arena queues rather than forcing `lane=bg`
  - changed assignment breadcrumbs so arena helpers now emit `[RTG][ARENA][ASSIGN]` instead of `[RTG][BG][ASSIGN]`
  - expanded queue-helper state clearing to wipe arena queue grace / leave / return markers as first-class lane state
  - standardized post-leave BG/arena retirement timing to a 30-second world-return window before logout
  - made final retire reason lane-specific so arena helpers retire as `rtg_arena_retire`
- `src/Bot/RtgBgQueuePlanner.cpp`
  - added arena-native planner keys (`rtg_arena_need`, `rtg_arena_team_need`, `rtg_arena_phase`)
  - stopped writing live arena demand through BG planner keys
  - explicitly clears old BG-lane arena residue when arena becomes dormant/orphaned so stale cross-lane truth cannot linger
  - normalized solo arena queue `9` to team-size `3` in the planner path too, matching the manager-side arena detection doctrine
- `src/Bot/RtgQueueLifecycle.cpp`
  - made demand/phase/team-need/pending/grace evaluation lane-aware for arena-managed helpers
  - arena helpers now consult `rtg_arena_*` ownership truth during retire evaluation instead of falling back to BG-only markers

### Behavioral doctrine after this pass
- BG lane remains `rtg_bg:*`
- Arena lane is now born, planned, assigned, dispatched, and retired as `rtg_arena:*`
- Arena no longer relies on shared BG keyspace for active demand truth
- BG and arena helpers now wait roughly 30 seconds after actually returning to world context before logout, which matches the requested scoreboard-safe leave-then-logout behavior better than the earlier near-immediate retirement

### Proof signals to demand next
- no new arena helper should log `add='rtg_bg:*:9'`
- arena helpers should log `[RTG][ARENA][ASSIGN]` and `lane=arena`
- after scoreboard/end or real-player disappearance, helpers should leave battleground/arena naturally and then log out only after the post-leave world-return delay
- arena dormant states should no longer leave reusable stale helpers hanging online for later arena cycles


## 2026-04-11 — RTG lane-native PvP spec doctrine for BG and arena helpers

### Summary
Pass 1 fixed the false-body problem by forcing BG/arena helpers to become real queue-ready PvP helpers before dispatch. The next live seam was spec quality: helpers were still selecting talents through the broad randombot class-spec probability tables, which made RTG PvP lanes inherit loose generic spec distributions. That produced weak or inappropriate PvP identities such as rogues not strongly preferring subtlety and druids entering PvP helper flow without a lane-native PvP template doctrine.

### Resolution
- added RTG lane-specific spec doctrine arrays in config/runtime:
  - `AiPlayerbot.RTG.BgClassSpecProb.<class>.<spectab>`
  - `AiPlayerbot.RTG.BgClassSpecIndex.<class>.<spectab>`
  - `AiPlayerbot.RTG.ArenaClassSpecProb.<class>.<spectab>`
  - `AiPlayerbot.RTG.ArenaClassSpecIndex.<class>.<spectab>`
- wired `PlayerbotFactory::InitTalentsTree()` to detect `rtg_bg:` / `rtg_arena:` add-data and use the RTG PvP selector instead of the generic random-spec selection path
- wired `PlayerbotFactory::InitTalentsByTemplate()` to resolve the lane-native PvP premade template index after the base tree tab is chosen
- default doctrine now strongly favors subtlety rogues for RTG PvP lanes and provides explicit feral/resto-biased druid defaults for BG/arena
- emitted explicit `[RTG][PVP][SPEC]` logs so future tests can confirm class, lane, tree tab, and premade-spec template choice during helper rebuild

### Important scope boundary
This pass fixes **spec doctrine**, not final item-family doctrine. It ensures BG/arena helpers are now rebuilt through lane-native PvP talent templates. Gear-family hard vetoes such as preventing feral from equipping caster leather are still the next pass and belong in item scoring / equipment assembly.

### Proof to demand next
- queued rogues in BG/arena should overwhelmingly emit subtlety PvP spec selections in the new `[RTG][PVP][SPEC]` breadcrumbs
- druid BG/arena helpers should now resolve through RTG PvP druid templates instead of broad random druid templates
- simultaneous BG+arena tests should no longer show helper quality drifting because the generic randombot spec tables happened to roll poorly
- next pass should lock PvP gear doctrine so feral/caster/healer item families match the lane-native PvP talent doctrine
