
---

## 🧾 RTG Brain Chronicle — Dispatch Capacity Pipeline Fix

### Problem
Active bots capped (~10–13) due to dispatch stall and helpers stuck in transition.

### Fix
- Added stale recovery (45s) to reset stuck helpers
- Replaced hard stall with soft throttling (trickle allowed)
- Added pipeline visibility logs

### Result
- Continuous acquisition under pressure
- Stuck helpers recycled
- Effective capacity aligns with MaxBots



## 2026-03-17 - compile repair: remove duplicate post-acquire dispatch recompute
- Follow-up repair after the dispatch-capacity surgical pass.
- The uploaded working tree still contained a second RTG post-acquire dispatch-budget recompute inside the acquire lane block.
- That block referenced outer-scope locals (`availableBots`, `onlineBotCount`, `pendingQueuedLogins`, `intervalCap`, `maxNewBots`, `loginBots`, `updateBots`) that only exist in the main update/login budget path.
- Result: compile failure plus downstream parser damage once the local structure became inconsistent around the surrounding `else` branch.
- Resolution: removed the duplicate post-acquire recompute entirely and kept the authoritative RTG dispatch budgeting only in the existing in-scope main budget section.
- Behavioral intent preserved: queue-managed helper dispatch still expands through the main RTG budget path without rewriting ownership, retirement, BG/RDF, or adaptive level logic.
