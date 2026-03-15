# RTG Brain Addendum — mod-playerbots

Module Version Target: 2.3.8ac-next
Brain Compatibility: 5.4.0

Commit Goal:
Repair RDF materialization reliability and BG finish_fill reacquisition while preserving recent collapse improvements.

--------------------------------

## Scope

IN SCOPE
• RDF helper acquisition / login / dispatch / stale protection
• BG startup fill, live_refill, finish_fill reacquisition
• Queue‑specific helper reservation accounting
• RDF vs BG helper separation

OUT OF SCOPE
• Playerbot AI changes
• Database schema changes
• Large queue planner rewrites

--------------------------------

## Core Design Rule

Helpers must have **service ownership**.

Two service lanes:

RDF lane:
mode = RDF
owner = RDF player
role = tank/healer/dps

BG lane:
mode = BG
queueId
team
bracket

Helpers in one lane cannot be reused by the other until their reservation ends.

--------------------------------

## RDF Behavior

Real player enters RDF
→ system acquires exact role bots
→ bots login with RDF reservation
→ bots dispatch only into RDF flow
→ bots protected from BG lifecycle logic

Expected group:
1 tank
1 healer
3 dps

--------------------------------

## Battleground Behavior

Real player enters BG queue
→ planner computes shortage
→ acquisition fills shortage
→ live_refill replenishes losses
→ finish_fill continues reacquisition until shortage is resolved

finish_fill must not be suppressed by global helper counts.

--------------------------------

## Known Failures To Fix

• finish_fill demand loops without helper login waves
• global online helpers suppress queue-specific reacquire
• RDF helpers treated as generic helpers
• BG helpers retired while queue still needs them
• reconnect turbulence stalls refill recovery

--------------------------------

## Acceptance Criteria

RDF
• role correct helpers appear
• helpers not stolen by BG
• no premature stale cleanup

BG
• startup fill works
• live_refill works
• finish_fill produces new helpers until shortage solved

Regression
• collapse behavior remains clean
• logs remain visible
--------------------------------

## RTG Queue Addendum 2.3.9-ad — Lane Isolation Refinement

Intent:
Preserve the shared RTG helper framework while isolating RDF and BG acquisition pressure more cleanly during mixed demand windows.

Patch focus:
• keep reserved RDF capacity inside the RDF lane for its first pass
• keep reserved BG capacity inside the BG lane for its first pass
• do not auto-donate unused RDF budget straight into BG before RDF has had a protected service window
• only expose leftover capacity as shared surplus after both lanes have attempted their reserved pass
• consume shared surplus with alternating lane preference so one service does not permanently win spillover

Why:
Previous logic proportioned capacity between RDF and BG, but then immediately donated leftover RDF capacity into BG by default because RDF was processed first. That behavior was safe for pure-BG demand bursts, but it weakened lane isolation during mixed demand windows and made BG the natural sink for any unspent budget.

Resulting model:
1. Compute reserved RDF budget and reserved BG budget.
2. Run RDF reserved pass.
3. Run BG reserved pass.
4. Merge unused reserved budget into shared surplus.
5. Use alternating lane preference to consume surplus without permanently biasing RDF or BG.

Important invariant:
This is not a planner rewrite.
It is an acquisition-lane refinement only.
Demand discovery, lifecycle cleanup, finish_fill priority, and orphan retirement rules remain intact.

Expected outcomes:
• RDF and BG stop stealing each other's first-pass reserved budget
• mixed RDF/BG pressure behaves more predictably
• BG no longer receives automatic first claim on unused RDF reservation
• spare capacity still gets reused after both lanes receive a fair attempt

## RTG Queue Addendum 2.3.9-ae — Dispatch Visibility + Stalled Add Recovery Checkpoint

Intent:
Create a safe checkpoint pass that improves RTG queue transparency and frees helpers that become stuck between acquisition and actual bot materialization.

Patch focus:
• mirror RTG runtime breadcrumbs to `server.loading` so queue control, acquire, login, dispatch, retire, and logout messages are visible in the same log stream as BG planner output
• emit explicit `[RTG][ACQUIRE][REQUEST]` when a queue-managed helper reservation is created
• emit explicit `[RTG][DISPATCH][ADD]` when `ProcessBot()` actually hands a queue-managed helper to `AddPlayerBot(...)`
• track queue-managed add requests with `rtg_add_requested`
• if a queue-managed add request never produces a player object within a safe stall window, emit `[RTG][DISPATCH][STALL]` and clear the stuck reservation so the system can reacquire instead of clogging
• clear `rtg_add_requested` on successful login, login failure, and safe logout

Why:
Recent logs showed BG planner demand advancing without the expected dispatch/runtime breadcrumb family being visible. There are two likely contributors:
1. planner logs already write to `server.loading`, but runtime breadcrumbs were still confined to the `playerbots` filter
2. queue-managed add requests may be getting stuck before actual player materialization, silently consuming helper slots and making the dispatch lane appear jammed

Important invariant:
This patch is an observability + unjam checkpoint.
It is not a planner rewrite, lane rewrite, or ownership rewrite.

Expected outcomes:
• startup control/acquire/login/dispatch breadcrumbs appear in the same worldserver-visible log stream as `[RTG][BG][PHASE]`
• if helper reservations stall before `AddPlayerBot` materializes them, the stall becomes visible
• stalled add slots are released instead of lingering and starving later helper demand
• this should help explain whether orphan residue is caused by invisible dispatch success, stalled bot materialization, or later queue-state cleanup problems


## RTG Queue Addendum 2.3.9-af — Standalone Control Ceiling Correction

Intent:
Ensure the RTG standalone queue-helper controller uses the RTG event-driven ceiling instead of the legacy random-bot ceiling.

Patch focus:
• change standalone control visibility from `MaxRandomBots` to `AiPlayerbot.RTG.EventDriven.MaxBots`
• use the RTG standalone helper ceiling when sizing fallback randombot account demand for queue-helper mode
• use the RTG standalone helper ceiling for RNDbot account-type assignment sizing in queue-helper mode
• use the RTG standalone helper ceiling for the early bot-initialization window when queue-helper mode is active

Why:
Recent startup logs still reported:
`[RTG][CONTROL] standalone queue-helper control active (MaxRandomBots=...)`

That is architecturally wrong for RTG queue-helper mode and can hide real capacity mismatches. In standalone queue-helper control, the event-driven helper system must be governed by `AiPlayerbot.RTG.EventDriven.MaxBots`, not by the legacy `AiPlayerbot.MaxRandomBots` random-bot ceiling.

Important invariant:
This is a focused ceiling-correction patch only.
It does not rewrite planner math, queue ownership, or dispatch logic.

Expected outcomes:
• startup control logs show the RTG helper ceiling instead of the legacy random-bot ceiling
• helper-account sizing logic no longer undershoots RTG standalone demand just because `MaxRandomBots` is lower
• early initialization timing aligns with RTG standalone helper capacity

## Checkpoint Addendum — RTG queue ownership correction

### Focus of this pass
- prevent same random-bot account from being reserved multiple times inside the same acquisition wave
- stop RDF abandonment from triggering merely because an owner is queued for battlegrounds
- only abandon RDF helpers when the owning real player has actually entered a battleground or arena map

### Behavioral intent
- pending and online helper reservations must respect account-level session exclusivity
- RDF helpers remain valid while the owner is only in battleground queue flow
- RDF helpers are cancelled only once the owner has crossed into battleground map state, because that is the point where the dungeon path is truly abandoned


## Queue checkpoint addendum – mixed BG/RDF owner arbitration

### Purpose
Prevent RDF helper acquisition from starving battleground startup when the same real player is queued for both systems at once.

### Rule
When battleground demand exists, RDF helper **acquisition** for an owner currently in battleground queue/invite/map flow is deferred rather than spawned immediately.

### Important distinction
- **Defer acquisition** when the owner is only in BG queue flow.
- **Abandon existing RDF helpers** only when the owner actually enters a battleground/arena map.

### Intent
This keeps battleground startup from losing helper capacity to RDF groups that are likely to be superseded by a near-immediate BG pop, while still avoiding premature RDF abandonment.


## Queue Fix Addendum - Mixed BG/RDF Startup Guard

- When battleground demand is active, queued RDF acquisition must not consume helper capacity needed to launch battlegrounds.
- Active dungeon/LFG-group support may still proceed, but plain queued RDF buckets should be deferred until BG startup pressure clears.
- This is an acquisition deferral rule, not an abandonment rule. Existing RDF helpers should still only be abandoned on true owner BG-map entry.
