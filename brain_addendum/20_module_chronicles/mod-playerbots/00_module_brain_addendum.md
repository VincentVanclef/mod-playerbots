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
