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


## 2026-03-22 — RDF acquisition materialization fix: immediate currentBots enrollment for queued helpers

### Runtime evidence
Live RDF logs showed a repeatable mismatch between:
- `[RTG][ACQUIRE][REQUEST]` count
- `[RTG][DISPATCH][ADD]` count

In failing cases, RTG would request a full 5-helper RDF composition but only a subset would ever enter the dispatch loop. The missing helpers then aged out as `[RTG][DISPATCH][STALL]` without ever receiving their initial dispatch breadcrumb.

### Root cause summary
Queued helpers were being marked with `add`/pending metadata during acquisition, but not immediately enrolled into `currentBots`.

That left helper materialization dependent on a later `GetBots()` rebuild pass and its current target window. In practice this created a race where some freshly acquired RDF helpers never entered the dispatch scan soon enough, then expired as false stall/fake account-pressure failures.

### Recovery action recorded in this chronicle
- queue-managed RTG helpers are now pushed into `currentBots` immediately when acquisition succeeds
- this keeps acquisition -> dispatch materialization aligned in the same control cycle instead of waiting for a later list rebuild

### Engineering lesson
For RTG event-driven helper lanes, successful acquisition is not complete until the helper is visible to the dispatch scan. Future passes should treat `add` metadata and `currentBots` enrollment as one materialization surface, not two loosely coupled stages.

## 2026-03-22 — RDF owner-fair dispatch pass: prevent fresh owners from stalling behind older unresolved lanes

### Runtime evidence
Recent RDF logs showed a clear mixed-owner starvation pattern:
- owner A would acquire a fresh 4- or 5-helper RDF batch
- dispatcher/login work would still keep draining an older owner B lane first
- owner A helpers would remain acquired but never receive `[RTG][DISPATCH][ADD]` before aging into `[RTG][DISPATCH][STALL]`

This created the false appearance of a single missing helper, but the deeper failure was owner-level starvation in the login/dispatch ordering.

### Root cause summary
`availableBots` was still inheriting `currentBots` list order directly. In event-driven RDF mode, that let older unresolved owner lanes sit at the front of the login/dispatch queue, while newly acquired helpers for a fresh owner remained later in the list.

The result was not just one helper stalling — it was a materialization fairness problem where a new owner with zero online helpers could be starved behind an older owner that already had online/pending helpers.

### Recovery action recorded in this chronicle
- event-driven `availableBots` ordering now prefers RDF owners with lower materialized pressure first
- ordering considers current online helper count and pending helper count per RDF owner
- a new debug breadcrumb `[RTG][RDF][DISPATCH_OWNER]` now exposes owner-level online/pending pressure at dispatch time
- `OnPlayerLoginError` definition was re-aligned with the header declaration using `char const* reason`

### Engineering lesson
For RTG RDF, fairness must be evaluated at the owner lane level, not just per-helper FIFO order. New owner lanes should get admission into dispatch/login before older unresolved lanes can consume the entire same-cycle budget.


## Chapter 25 — RDF Hybrid-Spec Role Compatibility
- **Date:** 2026-03-24
- **Subsystem:** RTG Queue System / RDF Role Resolution
- **Context:** Live RDF tests showed one helper logging in successfully but failing immediate join dispatch while replacement DPS helpers were requested. User also observed dual feral druids where one should tank and the other should dps.
- **Situation:** The RDF pipeline was still treating actual/offline role truth as a single exact role for a given spec tab. That works for rigid specs, but it breaks hybrid specs like feral druid where the same spec legitimately satisfies tank or dps lane assignment.
- **Hypothesis:** Exact-role comparisons were rejecting hybrid-spec helpers for desired dps assignment, leading to online bot occupancy without correct queue participation and misleading replacement pressure.
- **Experiments:** Added spec-role compatibility helpers so offline candidate selection and live join validation can ask whether a spec can perform a role, not only what its default primary role is.
- **Findings:** Feral druids must be treated as compatible with both tank and dps RDF assignment. Exact-match role gates were too strict and caused false runtime_role_mismatch / login_success result=0 style failures.
- **Regression:** None intended; rigid specs continue to map to their single compatible role.
- **Next Investigation:** If a hybrid spec still fails to join under the assigned role, inspect whether the client role-mask packet is being overridden after join submission rather than role-resolution rejecting it.
- **Status:** Applied.

## Chapter 26 — RDF Join-Failure Hard Recycle + Planner Role Authority
- **Date:** 2026-03-24
- **Subsystem:** RTG Queue System / RDF Login Materialization
- **Context:** Live RDF tests still showed one helper logging in but not actually entering queue participation, followed by replacement acquire attempts and misleading account-pressure messages.
- **Situation:** A queue-managed helper could log in successfully, fail the immediate `lfg join` attempt, and still occupy lane ownership long enough for the planner to request additional helpers. Hybrid specs could also submit their default role instead of the planner-assigned role even when the spec was compatible with the assigned role.
- **Hypothesis:** Two remaining seams existed: (1) `login_success result=0` was not treated as a hard failure, and (2) RDF join role-mask generation still preferred actual role over planner-assigned compatible role for hybrid specs.
- **Experiments:** Added immediate RDF join dispatch on helper login, added hard recycle/logout on immediate join failure with a short retry cooldown, skipped recently failed helpers during acquire, and changed `LfgJoinAction::GetRoles()` to return the planner-assigned role whenever the live spec can legitimately perform that role.
- **Findings:** RTG RDF must treat failed immediate join as slot invalidation, not a soft retry. Also, spec determines capability while planner determines assigned lane role; hybrid specs like feral druid need planner role authority to prevent dual-tank/dual-dps drift.
- **Regression:** None intended; incompatible specs still fall back to actual role and are rejected later by compatibility guards.
- **Next Investigation:** If a logged-in helper still fails to materialize after these changes, inspect server-side LFG state mutation immediately after `CMSG_LFG_JOIN` submission rather than selection/role resolution.
- **Status:** Applied.


## Chapter 31 — RDF helper over-acquire hold gate
**Date:** 2026-03-24
**Subsystem:** mod-playerbots / RTG RDF acquisition
**Context:** Live RDF testing showed 4 helpers acquired and dispatched for a solo owner, then a 5th DPS helper was still requested before the earlier four had fully resolved into queue state.
**Situation:** The queue system was still willing to acquire extra RDF helpers while the owner already had enough queued+assigned helpers to fill the party. This produced misleading account-pressure errors and slot thrash.
**Hypothesis:** Acquisition was keying too aggressively off per-role transient state and not strongly enough off total owner-lane occupancy.
**Experiments:** Added an owner-lane hold gate in the RDF acquisition path so no further helpers are acquired once queued+assigned helper ownership already meets the total helper target for that owner.
**Findings:** This should prevent 5th-helper over-acquire for a 4-helper solo RDF owner and force the system to recycle/repair an existing bad helper before attempting further login.
**Regression:** None intended; this only suppresses extra acquisition when the owner lane is already fully occupied by queued+assigned helpers.
**Next Investigation:** If a lane is still stuck after this, the next seam is identifying and retiring the exact already-owned helper whose actual queued role does not match the assigned role.
**Status:** Implemented


## Chapter 36 — RDF finalization: deterministic enter-dungeon pump
- **Date:** 2026-03-23
- **Subsystem:** mod-playerbots / RTG RDF queue finalization
- **Context:** Live tests showed four helpers acquiring, dispatching, logging in, and reaching the ready dialog, but no helper actually entered the dungeon.
- **Situation:** The RDF lane was no longer failing at selection or initial queue join. The remaining blocker was the final enter-dungeon step after proposal acceptance.
- **Hypothesis:** The queue system had proposal acceptance markers (`rtg_lfg_proposal_lock`, `rtg_lfg_accept_sent`) but no deterministic manager-side phase that converted accepted proposals into `lfg teleport` actions.
- **Experiments:** Audited the online-queued helper maintenance loop and confirmed that accepted proposal state was not consumed anywhere in `RandomPlayerbotMgr.cpp`. Added a deterministic proposal-latched teleport phase that attempts `lfg teleport` while a queued RDF helper has proposal/accept state and is not yet inside a dungeon map.
- **Findings:** The RDF lane previously depended on opportunistic AI trigger timing for the final enter-dungeon step. That was weaker than the BG lane lifecycle and allowed ready dialogs to expire even after correct queue assembly. The new pump keeps accepted helpers in a proposal-resolution phase and drives explicit teleport attempts until map transition.
- **Regression:** None intended; teleport attempts are gated by proposal/accept state and suppressed once the bot is already inside a dungeon.
- **Next Investigation:** If a helper still fails to enter after deterministic teleport attempts, inspect whether the specific group/leader state expected by the core LFG teleport path differs from current RTG helper group composition.
- **Status:** Integrated.


## Chapter 12 - RDF Finalization Button-Path Audit
- Date: 2026-03-24
- Subsystem: RTG Queue System / RDF finalization
- Context: Bots were queueing correctly, reaching accepted proposal state, and manager-side teleport attempts were firing, but all attempts showed `grouped=0`. Screenshots confirmed the ready dialog was visible and the intended action was the normal `Enter Dungeon` path.
- Situation: The RTG manager was still trying to drive the final step too early. Bots were not yet inside an actual LFG group when `lfg teleport` was attempted, so the last-mile path could not succeed even though queue and proposal stages had already completed.
- Hypothesis: The clean separation is to let RTG own getting helpers into RDF-ready state and let the core-ready-dialog path own the final enter transition. Manager-side finalization should therefore wait for real LFG grouping before attempting the normal enter-dungeon packet path.
- Experiments: Tightened the manager-side finalization gate so proposal-latched bots only attempt `lfg teleport` after `bot->GetGroup()` exists and `group->isLFGGroup()` is true. Added `[RTG][RDF][WAIT_GROUP]` breadcrumbs and made `LfgTeleportAction` return false for queued RDF helpers that are not yet in an actual LFG group.
- Findings: The prior implementation was over-eager and converted accepted proposal state into repeated teleport attempts even when group formation had not completed. That created noisy `result=1` breadcrumbs with `grouped=0`, obscuring the real blocker.
- Regression: None intended; this pass removes premature finalization attempts instead of broadening behavior.
- Next Investigation: If bots still fail after this pass, inspect the exact core-side ready-dialog handler path used by the Enter Dungeon button and mirror that handler/opcode path directly rather than relying on generic teleport semantics.
- Status: In progress.
