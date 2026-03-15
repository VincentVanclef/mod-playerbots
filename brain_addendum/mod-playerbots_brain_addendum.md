# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8g
RTG Brain Compatibility Version: 5.4.0
Commit Title: Prevent same-account helper collisions and retire abandoned RDF fillers during BG diversion
Commit Description: Repairs the refill stall now that dispatch is working by preventing multiple simultaneous helper reservations from reusing the same random-bot account, and by retiring RDF fillers whose real owner has diverted into battleground flow.

--------------------------------

## Module Purpose

This revision addresses the next live bottleneck discovered after RTG standalone helper targeting was corrected. Startup launch and initial BG fill now work, but mature refill could stall because helper acquisition was still allowed to reserve multiple characters from the same account at once. In parallel, RDF fillers could remain idling in the world after their real owner abandoned RDF by entering battleground flow.

--------------------------------

## Architecture Overview

- `RandomPlayerbotMgr` remains the runtime coordinator for helper acquisition.
- RTG queue-managed helper reservations must now obey a one-active-character-per-account rule during a login wave.
- RDF fillers remain isolated from BG fillers, but abandoned RDF ownership is now recognized faster when the owner diverts into battleground flow.
- Existing RTG lifecycle and safe-retire doctrine stay in force.

--------------------------------

## Runtime Control Path

real demand detected
→ helper acquisition builds RDF/BG buckets
→ acquisition now seeds a busy-account set from already-online/pending helpers
→ each new reservation must use an unused random-bot account
→ helper logs in and dispatches normally
→ RDF stale sweep now abandons idle RDF fillers when their real owner is actively in battleground flow

--------------------------------

## Data Structures

No new persisted schema.

Runtime-only rules added:
- busy random-bot account set seeded from `currentBots`
- RDF owner battleground-flow detection for player owners

--------------------------------

## Config Interface

No new config keys in this revision.

Relevant existing settings:
- `AiPlayerbot.RTG.EventDriven.Enable`
- `AiPlayerbot.RTG.EventDriven.MaxBots`
- `AiPlayerbot.RTG.QueueGraceSeconds`
- `AiPlayerbot.RTG.QueueOwnership.Enable`
- `AiPlayerbot.RandomBotAccountCount`

--------------------------------

## Database Structures

No SQL changes.

--------------------------------

## Integration Points

- `src/Bot/RandomPlayerbotMgr.cpp`
- existing RTG queue helper event markers (`add`, `rtg_lfg_pending`, `rtg_bg_pending`, `rtg_add_requested`)

--------------------------------

## Lifecycle Model

- Queue-managed helper reservations may not compete against themselves through same-account collisions.
- A helper reservation that would reuse an account already online/pending is now skipped so refill can continue using other accounts.
- RDF fillers that never entered RDF and whose real owner is now in battleground flow are marked abandoned and retired through the safe logout path.

--------------------------------

## Known Constraints

- Owner diversion detection in this pass is strongest for solo-player owners because it checks connected player battleground state directly.
- Group-owner RDF diversion can still be improved later if needed, but this pass fixes the observed live test case with two solo players.
- This revision intentionally avoids rewriting BG refill math or the stall watchdog.

--------------------------------

## Future Evolution Hooks

- explicit helper-account reservation telemetry (`[RTG][ACQUIRE][ACCOUNT]`)
- group-owner RDF diversion detection
- smarter reuse backoff for recently stalled helper accounts

--------------------------------

## Files Modified In This Revision

- `src/Bot/RandomPlayerbotMgr.cpp`
- `brain_addendum/mod-playerbots_brain_addendum.md`

--------------------------------

## Behavioral Changes In This Revision

- Helper acquisition no longer reserves multiple queue-managed bots from the same account in one wave.
- Refill waves should no longer self-stall because several pending helpers were all tied to the same account.
- Idle RDF fillers now retire sooner when their real owner has switched into battleground flow instead of remaining queued for a dungeon that no longer has an owner.

--------------------------------

## Test Plan

1. Keep `AiPlayerbot.RandomBotAutologin = 0`.
2. Enable RTG standalone queue-helper mode.
3. Queue one real player for WSG + RDF and another for Eye of the Storm + RDF.
4. Confirm startup launch still fills both battlegrounds.
5. Inspect `[RTG][ACQUIRE][REQUEST]` and verify the same account ID is no longer repeated multiple times inside a single refill wave.
6. Confirm `live_refill` and `finish_fill` continue acquiring after the first match launch instead of degrading into repeated `[RTG][DISPATCH][STALL]` bursts.
7. Watch for `[RTG][LFG][ABANDON]` when an RDF filler owner diverts into battleground flow.
8. Confirm abandoned RDF fillers retire and no longer linger in the world unnecessarily.

--------------------------------

## Notes For RTG Brain Ingestion

This revision formalizes two more RTG queue-assistance rules:
1. queue-managed helper acquisition must respect one-online-character-per-account reality during reservation, not just at eventual login time;
2. RDF ownership should collapse quickly when the real owner abandons RDF by entering battleground flow, otherwise stale fillers can consume helper capacity and obscure refill health.
