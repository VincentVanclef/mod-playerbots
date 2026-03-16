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

---

## Addendum — same-tick dispatch refresh after queue acquisition

### Patch focus

This revision fixes a regression where RTG queue helpers were being successfully **acquired** into `currentBots`, but the same world tick still dispatched against an older pre-acquisition `availableBots` snapshot.

### Problem observed

Under multi-queue pressure:

- the first battleground in the tick could acquire and dispatch normally
- later battlegrounds or RDF helpers could also be acquired in the same tick
- but those later helpers were **not visible to ProcessBot login dispatch until the next tick**
- meanwhile the stall watchdog kept aging those helpers as pending queue-managed `add` entries
- result: logs showed large `ACQUIRE` waves, but only the first batch produced `DISPATCH][ADD]` and `LOGIN`

This matched the live symptom of:

- first BG starts
- second / third BG stays stuck in `pop_or_invite`
- `finish_fill` or `live_refill` helpers keep appearing later
- many pending helpers eventually emit `DISPATCH][STALL]`

### Root cause

`UpdateAIInternal` built these dispatch-side views **before** `AddRandomBots()` ran:

- `availableBots`
- `availableBotCount`
- `onlineBotCount`

When `AddRandomBots()` appended newly acquired RTG helpers into `currentBots`, the dispatch loop still used the stale snapshot built earlier in the tick.

A previous hotfix attempt tried to refresh dispatch state, but placed that logic inside `AddRandomBots()` using locals that only exist in `UpdateAIInternal`, which caused the compile failure around undeclared identifiers such as:

- `availableBots`
- `availableBotCount`
- `onlineBotCount`
- `pendingQueuedLogins`
- `intervalCap`

### Resolution

This patch does two things:

1. removes the misplaced post-acquire dispatch-budget block from `AddRandomBots()`
2. refreshes dispatch state in the correct scope, inside `UpdateAIInternal`, immediately after `AddRandomBots()` can mutate `currentBots`

Refreshed values now include:

- `availableBots = currentBots`
- `availableBotCount = availableBots.size()`
- `onlineBotCount = playerBots.size()`

A new trace was also added:

- `[RTG][DISPATCH][REFRESH] availableBotCount=... onlineBotCount=... pendingDispatchable=...`

### Intent

- make newly acquired BG/LFG helpers dispatchable in the **same tick** they are reserved
- stop second/third queue waves from being invisible until a later update
- reduce false `DISPATCH][STALL]` cleanup for healthy helpers
- preserve existing BG phase priority logic while restoring immediate dispatch correctness

### Expected behavioral change

When three battleground queues arrive in rapid succession, logs should now show:

- `ACQUIRE` for queue 4, queue 3, and queue 2 in the same tick window
- one `DISPATCH][REFRESH]` line showing the refreshed pending helper count
- `DISPATCH][ADD]` and `LOGIN` continuing beyond the first BG batch instead of stopping at ~13 bots

### Campaign status

This is a **dispatch visibility / snapshot freshness** correction, not a queue formula rewrite.

If startup still stalls after this patch, the next likely layer is no longer stale dispatch state — it will be one of:

- offline account reuse collisions across lanes
- queue ownership residue not clearing fast enough
- battleground queue metadata not being revalidated after launch transitions

---

## Addendum: RTG Queue Recovery — compile-safe event-driven return restoration

### Version note

This addendum documents the recovery applied after a bad merge damaged the closing structure of the RTG event-driven helper block inside `RandomPlayerbotMgr::AddRandomBots()`.

### Symptom

The module stopped compiling with parser-style errors beginning near the end of the RTG event-driven acquisition branch, including messages like:

- `expected expression`
- `function definition is not allowed here`
- large cascades of false downstream syntax errors

Those errors were not independent defects. They were the normal fallout of one corrupted control-flow boundary near the bottom of the RTG helper acquisition block.

### Root cause

The RTG event-driven branch is designed to short-circuit out of `AddRandomBots()` once RTG-managed acquisition has finished its own demand calculation and helper reservations.

A broken merge removed that intended early return shape and allowed the legacy alliance/horde phased login block to be parsed as if it belonged to the RTG branch. That shifted the surrounding `if / else` structure out of alignment and caused the compiler to interpret subsequent method definitions as nested inside `AddRandomBots()`.

### Resolution

The recovery restored the known-good tail structure of the function:

1. preserve RTG event-driven acquisition result logging
2. preserve missing-bot timer handling for unresolved RTG demand
3. restore the intended early return from the RTG branch
4. restore the legacy phased login branch as the fallback non-RTG path
5. restore the outer `else` that clears the missing-bot timer when enough bots already exist

### Why this mattered to queue testing

This recovery is a **structural compile repair**, not the final queue-behavior fix by itself.

Without this restoration, every subsequent queue test was invalid because the build was not structurally trustworthy.

With the RTG event-driven branch closed correctly again, future startup / dispatch / finish-fill debugging can once again be reasoned about from real runtime behavior instead of merge corruption.

### Expected result from this recovery

After this fix, the module should:

- compile past the brace/parser failure region
- preserve RTG event-driven helper acquisition semantics
- preserve legacy non-RTG fallback login behavior
- allow the next queue test to focus on real startup starvation rather than syntax damage

### Next investigation layer

If multi-BG startup still plateaus at ~13 bots after compile recovery, the next live causes to inspect remain:

- dispatch starvation after first-BG launch
- finish-fill consuming capacity before startup lanes finish
- pending helper ownership / queue residue preventing same-account reuse
- lane fairness between queue 2 / queue 3 / queue 4 under shared BG ceilings


---

## Addendum: BG ceiling pressure and RDF lane suppression recovery

### Test result that triggered this addendum

After compile recovery and same-tick dispatch refresh were restored, RTG successfully launched and deep-filled **three simultaneous battleground lanes**:

- queue 2 advanced into `finish_fill` around `10v10`
- queue 3 advanced into `finish_fill` around `13v13`
- queue 4 advanced into `finish_fill` around `13v13`

This proved the earlier startup starvation bug was largely resolved.

However, the next test exposed two remaining pressure points:

1. battleground demand was now strong enough to push near the configured BG helper ceiling
2. RDF/LFG startup helpers could still be delayed or suppressed while BG demand was present

### Root cause A: battleground helper ceiling is now too conservative for RTG's desired multi-BG state

The previous BG helper ceiling of `77` was sufficient for earlier startup goals, but the repaired planner now sustains deeper finish-fill targets across multiple concurrent battlegrounds.

At RTG scale, three active BGs can legitimately consume most of that allowance before RDF receives practical breathing room.

### Root cause B: LFG reserved capacity still honored BG presence too aggressively

Although lane reservation already split total need into separate LFG and BG slices, the LFG fill path still skipped non-active dungeon buckets whenever battleground demand existed:

- reserved LFG capacity could exist
- but fresh RDF startup buckets were skipped
- only already-active dungeon buckets were allowed through

That meant RDF startup could appear starved even when the scheduler had already reserved LFG-side budget for it.

### Resolution in this revision

This revision applies two linked corrections:

1. **LFG reserved lane recovery**
   - fresh RDF/LFG startup buckets are now allowed to consume their own reserved LFG slice even while BG demand is active
   - BG pressure may still compete for shared surplus later, but it no longer nullifies the LFG lane itself

2. **Configuration baseline uplift**
   - `AiPlayerbot.RTG.EventDriven.MaxBots` default raised from `120` to `156`
   - `AiPlayerbot.RTG.EventDriven.BattlegroundMaxBots` default raised from `77` to `96`
   - `AiPlayerbot.RTG.EventDriven.LfgMaxBots` remains `60`

### Why these numbers

This new baseline is meant to fit RTG's observed operating goal:

- three battlegrounds can deepen past their minimal launch thresholds
- RDF still retains a protected helper envelope
- total RTG helper pressure remains bounded by a single master cap

The intent is not infinite growth. It is to stop healthy multi-BG activity from crowding RDF out of existence.

### Engineering interpretation

The system has crossed an important threshold:

- before, the problem was **startup starvation**
- now, the problem is **successful throughput creating real capacity pressure**

That is progress.

The correct next-phase design is therefore:

- preserve independent lane reservation
- preserve startup-first suppression of BG `finish_fill` when necessary
- allow RDF to use its own lane even under BG load
- raise the documented RTG helper envelope to match the realm's real concurrency target

### Expected next test outcome

With this revision in place, the next live run should show:

- all three battlegrounds still launching and deep-filling
- RDF helper startup no longer blocked merely because BG demand exists
- fewer cases where BG finish-fill consumes the practical envelope before RDF gets to assemble

If starvation still appears after this revision, the next diagnostic layer is no longer planner order or reserved-lane suppression. It will most likely be:

- offline character availability versus current account pool composition
- queue retry / invitation acceptance losses inside a specific battleground lane
- per-lane helper reuse or residual ownership timing

## Addendum — grouped RDF recovery after tank loss and quieter orphan residue logging

### Problem observed
A real-player RDF run could successfully form and enter the dungeon, but if a bot tank disappeared early, the remaining bot party members would no longer participate correctly in the next queue cycle with the player.

The practical symptom was:

- the player re-queued from an active dungeon/group state
- replacement RDF helpers could be acquired and dispatched
- but grouped helper bots stopped contributing to the role-check / regroup flow
- meanwhile battleground orphan-residue logging kept repeating every planner pass after matches had already drained

### Root cause
This was not the old lane-starvation issue.

It split into two narrower problems:

1. **Grouped RDF support regression**
   A safety guard added to suppress unwanted LFG behavior in real-player groups was too broad. It also blocked legitimate RTG queue-managed RDF helpers from answering group role-checks after regroup / replacement scenarios.

2. **Active dungeon demand recognition too narrow**
   RTG LFG demand tracking treated active-dungeon ownership as trustworthy mostly when the group still looked like an LFG group. After mid-run disruption, a real-player-led dungeon party could still need replacement helpers even if the group shape was no longer a pristine LFG-group state.

3. **Planner clear-log spam**
   `orphan_queue_residue` logging was emitted every planner sweep, which inflated logs without adding new signal.

### Structural correction
This revision does three things:

- allows **queue-managed RDF helpers** to continue participating in `LfgRoleCheckAction` even when grouped with a real player
- broadens RTG active-dungeon recognition so a **real-player group inside a dungeon map** still counts as valid active RDF demand for replacement planning
- throttles repeated `orphan_queue_residue` planner logs so the signal remains visible without flooding the log stream

### Why this matters
This keeps the intended RTG behavior intact:

- random ambient bots should not inject themselves into ordinary real-player groups
- **assigned / queue-managed RDF helpers** must still help finish the recovery path after a broken dungeon party or lost tank/healer scenario

That distinction is essential.

### Expected next-test behavior
On the next run we should expect:

- regrouped RDF bots to answer role-check again after a replacement cycle
- active dungeon ownership to continue generating helper demand when the player is still inside the instance with a real-player-led group
- cleaner battleground cleanup logs after matches end

### Remaining watchpoints
The following are still worth validating in the next live pass:

- whether replacement RDF helpers are being invited and role-checking promptly enough after a tank drop
- whether any remaining RDF refill failures are now due to account-pool eligibility rather than grouped-helper suppression
- whether BG finish-fill ceilings still plateau early because of true account exhaustion versus reusable-helper timing
- whether retirement pacing should be tightened further once grouped RDF recovery is stable


## Addendum — fast no-player retirement, orphan queue collapse hardening, and live-config reconciliation

### Problem observed
The RTG queue engine is now capable of filling three battlegrounds plus RDF, but the remaining failures shifted from startup/fill into **post-demand cleanup** and **config drift**:

- battleground fill hit practical helper ceilings rather than failing to start
- orphan battleground residue could keep planner state alive long after the real demand was gone
- queue-managed helpers could remain online too long after all real players logged out
- `playerbots.conf.dist` and the live RTG config block drifted apart, while several documented RTG keys were not actually being loaded by `PlayerbotAIConfig.cpp`

### Root cause
This phase exposed three separate issues:

1. **Real config drift**
   The mod shipped RTG config documentation for keys such as smart queue and queue ownership controls, but the loader only consumed a subset of them. That made the config surface look richer than the effective runtime surface.

2. **No-player shutdown depended too heavily on the global disabled-without-real-player timer**
   Queue-managed helpers were eventually removed, but not quickly enough for RTG's event-driven operating model.

3. **Orphan battleground residue was still treated as planner-relevant noise**
   The planner logged residue repeatedly and could continue re-evaluating stale battleground helper state instead of collapsing it decisively.

### Resolution in this revision
This revision applies four linked corrections:

1. **Config loader reconciliation**
   `PlayerbotAIConfig.cpp` now loads the RTG keys that are actually used by runtime logic:
   - `AiPlayerbot.RTG.SmartQueue.Enable`
   - `AiPlayerbot.RTG.DemandCheckSeconds`
   - `AiPlayerbot.RTG.QueueOwnership.*`
   - `AiPlayerbot.RTG.EventDriven.NoPlayersRetireDelay`
   - `AiPlayerbot.RTG.EventDriven.DungeonFinishedLogoutDelay`

2. **Demand-check cadence becomes configurable**
   The hardcoded RTG queue scan interval is replaced by `AiPlayerbot.RTG.DemandCheckSeconds`.

3. **Fast queue-helper retirement when no real players remain**
   Once the no-player delay expires, queue-managed RTG helpers are pushed toward safe logout immediately instead of waiting for the much slower generic population shutdown path.

4. **Harder orphan-residue collapse**
   Orphan battleground residue is still logged, but it is throttled more heavily and is no longer allowed to keep the planner treating stale state as active demand.

### Why this matters
This is the correct late-phase correction for the RTG queue system. Earlier passes were about making helpers appear. This pass is about making them **disappear correctly** and making the config surface truthful.

### Expected next test behavior
The next live run should show:

- battleground and RDF fill still functioning as before
- less stale `orphan_queue_residue` log churn
- faster queue-helper retirement after all real players log out
- a `playerbots.conf.dist` RTG section that matches the settings the code really consumes

### Remaining watchpoints
Still validate the following next:

- whether `RandomBotAccountCount = 1200` is enough once battleground caps are raised further
- whether RDF replacement helpers after a mid-run tank loss now recover cleanly with the current grouped-LFG behavior
- whether any remaining long-tail helper persistence is battleground match-duration limited rather than planner-lifecycle limited
- whether RTG wants even shorter post-dungeon helper retirement once replacement stability is confirmed

---

## Addendum: explicit RNDbot account assignment, RDF deserter scrubbing, and independent LFG teardown

### Triggering regression set

A later live test showed a strong regression pattern even after the earlier queue-capacity uplift:

- only one battleground reliably formed while the other startup lanes remained a few bots short
- RDF could assemble once, but replacement behavior after a bot dropout was unreliable
- at least one bot picked up **Dungeon Deserter**, making that helper account effectively useless until the aura expired
- RDF helpers could remain online in the world after the real player left the dungeon flow early
- observed helper population repeatedly plateaued around the mid-30s despite a much larger configured pool

### Root cause A: explicit `RandomBotAccountCount` was not being honored as an assignment target

The account-type assignment code still derived RNDbot account allocation mainly from the formula:

- `ceil(RTGMaxBots / available_chars_per_account)`

That meant the realm could have a very large **existing** randombot account pool, but only the small formula-derived subset would be marked as type-1 RNDbot accounts and therefore available for event-driven helper acquisition.

In practice, this creates a false account-starvation ceiling that looks like:

- helper acquisition works initially
- around ~30-40 helpers the system starts claiming it needs more accounts
- battleground startup lanes stay slightly short even though many unassigned randombot accounts already exist

### Resolution A

When `AiPlayerbot.RandomBotAccountCount` is explicitly configured, it is now treated as an **authoritative RNDbot assignment target**, not only as an account-creation hint.

This means RTG standalone queue-helper mode can actually use the configured account pool instead of only the small formula-derived assignment subset.

### Root cause B: LFG cleanup was incorrectly coupled to total RTG demand disappearance

The earlier cleanup block only tore down idle RDF helpers when **both** conditions were false:

- no LFG demand
- no BG demand

That is too strict for RTG multi-lane operation. If battleground demand is still active, abandoned RDF helpers could stay online even though the dungeon lane itself is dead.

### Resolution B

LFG idle teardown now triggers whenever **LFG demand itself has vanished**, regardless of whether battleground demand is still active elsewhere.

That restores correct lane independence:

- battleground activity no longer keeps dead RDF helpers alive
- leaving an RDF flow early should release those helpers much sooner
- freed accounts can be reused by the next queue wave

### Root cause C: queue penalties were not being scrubbed aggressively enough for recycled RTG helpers

Although deserter cleanup already existed in some queue paths, live behavior still showed a bot receiving **Dungeon Deserter** and becoming unusable for the next replacement pass.

### Resolution C

Queue debuff cleanup is now reinforced by:

1. expanding the common RTG queue-debuff scrubber to also remove the alternate dungeon-deserter aura
2. clearing queue penalties immediately on RTG bot login before helper assignment resumes
3. clearing `rtg_dungeon_active` as part of safe helper logout queue-state teardown

### Intended behavioral change after this revision

Expected next-pass behavior:

- explicit large randombot pools can actually be used by event-driven helper acquisition
- second and third battleground startup lanes should no longer stall early from artificial account scarcity
- RDF helpers abandoned after group collapse / real-player exit should log out independently of BG activity
- recycled RDF helpers should not come back poisoned by Dungeon Deserter

### Scope note

This addendum is intentionally **account-pool / LFG lifecycle / debuff hygiene** focused.

It does **not** claim the entire queue system is finished. If startup still misses by a few bots after this revision, the next likely layer is no longer type-1 account starvation — it will more likely be one of:

- remaining queue residue timing
- queue-retry revalidation gaps
- battleground ownership cleanup lag
- RDF replacement timing after abrupt member loss
