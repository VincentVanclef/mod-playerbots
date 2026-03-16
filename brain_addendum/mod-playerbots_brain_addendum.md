# mod-playerbots brain addendum

## Commit Title
mod-playerbots: align RTG dispatch budget with pending queue-managed logins

## Commit Description
This patch fixes the RTG queue-system formula mismatch where acquisition could correctly reserve many helper bots in one pass, but the downstream dispatch/login budget still throttled actual bot materialization to the legacy `randomBotsPerInterval` cadence.

### Problem
The queue planner and acquisition layer were issuing large valid helper waves for battleground demand, but `UpdateAIInternal()` still limited login processing using legacy per-interval bot cadence and an extra `botLoading.empty()` gate. In practice this caused:

- many `[RTG][ACQUIRE][REQUEST]` lines
- only a few `[RTG][LOGIN]` successes per cycle
- repeated `[RTG][DISPATCH][STALL]` for correctly requested helpers
- false `RandomBotAccountCount` exhaustion messages even when the real problem was dispatch throttling

### Fix
- compute `pendingQueuedLogins` from current RTG queue-managed helpers that have `add` set but are not yet loaded
- raise `intervalCap` to at least that pending RTG login count while event-driven mode is active
- force `loginBots` to cover the pending RTG queue-managed helpers, bounded by `maxNewBots`
- rebalance `updateBots` downward if needed so the interval budget prioritizes queue fulfillment
- remove the unnecessary `botLoading.empty()` gate so the login loop no longer blocks an entire RTG helper wave behind a single in-flight async login
- add `[RTG][DISPATCH][BUDGET]` logging so dispatch math is visible in logs

### Intent
The RTG queue system should follow this formula chain consistently:

1. planner computes unresolved team demand
2. acquisition reserves helper identities for that demand
3. dispatch/login budget must be large enough to materialize those already-reserved helpers promptly
4. queue join logic consumes the materialized helpers

This patch repairs stage 3 so it no longer silently reintroduces the old low-cadence random-bot throttle into RTG queue assistance.
