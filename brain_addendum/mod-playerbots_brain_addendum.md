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
