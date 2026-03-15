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

## Queue Repair Addendum — 2.3.8ad-next

Targeted pass completed against observed logs:

• finish_fill now sorts ahead of startup/pop queues so active-match reacquire is not starved by new queue seeds
• BG helper acquisition now allocates in round-robin passes across BG buckets instead of exhausting one bucket before servicing others
• orphan queued BG helpers with no remaining real demand are now force-retired through safe logout path instead of lingering and repeatedly producing orphan_queue_residue planner spam

Expected log changes:

• active finish_fill buckets should continue producing helper login waves even when another BG queue enters pop_or_invite at the same time
• queue=4 style mature refill should no longer be repeatedly pre-empted by queue=2 startup demand
• repeated orphan_queue_residue spam should collapse after stranded helpers are logged out
