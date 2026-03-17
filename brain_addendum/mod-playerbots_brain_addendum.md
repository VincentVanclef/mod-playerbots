
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


---

## 🧾 RTG Brain Chronicle — Dispatch Capacity Pipeline Surgical Recovery Pass (2026-03-17)

### Context
This pass was performed directly against the existing `RandomPlayerbotMgr.cpp` with the standing RTG queue architecture, ownership, retirement, and adaptive-level systems preserved in-place. The target was the recurring field symptom where BG helper flow would appear healthy at first, then flatten around roughly 10–13 active helpers even though planner demand, helper ownership, and offline account supply still indicated more dispatch capacity should have remained available.

### Diagnosis
The stall signature matched a compound capacity choke rather than a single planner failure:

1. **Transition watchdog coverage gap**
   - Reserved / world-idle helpers could remain counted as pending ownership participants without being actively re-dispatched or safely released.
   - Offline helpers stuck beyond the dispatch stall window could continue consuming effective capacity until later cleanup paths happened to touch them.

2. **Per-lane helper cap acting like a hidden ceiling**
   - BG lane pending ceilings around the old fixed `10/12` range were capable of freezing mature-fill / finish-fill progression under sustained demand.
   - This presented externally like a realm-wide active-helper ceiling even when global RTG and account capacity were still available.

3. **Post-acquire dispatch headroom mismatch**
   - Same-tick dispatch after acquisition used `rtgEventMaxBots` instead of the already computed live `maxAllowedBotCount` / `rtg_target`, creating a second implicit dispatch choke in heavy queue waves.

### Surgical Fix Applied
- Expanded transition-state auditing to include `Reserved` and `WorldIdle`, not just queued/invited/releasing states.
- Added **offline stale-transition recovery** that safely releases helpers whose request aged past the dispatch stall threshold without ever fully materializing.
- Added **online watchdog re-dispatch** for BG helpers that are in a stale transition but still alive and recoverable.
- Preserved queue ownership semantics by reassigning queue ownership only when a real re-dispatch succeeds.
- Preserved safe retirement semantics by leaving pending-retire helpers in release state rather than force-recycling them.
- Replaced the most restrictive fixed BG per-lane pending limit behavior with a bounded dynamic ceiling that scales with the configured RTG helper budget.
- Corrected post-acquire dispatch headroom to honor live `maxAllowedBotCount` rather than the raw event cap.

### Resulting Behavior Contract
- Existing working BG/RDF logic remains intact.
- Ownership + retirement flows remain authoritative.
- Adaptive level handling remains untouched.
- Helpers stuck in stale transition states no longer hold the pipeline hostage indefinitely.
- Finish-fill and high-pressure BG dispatch should continue progressing under load instead of flattening near the earlier pseudo-cap.

### Notes For Next Pass
Priority validation logs to inspect after deployment:
- `[RTG][RECOVER][DISPATCH]`
- `[RTG][RECOVER][RELEASE]`
- `[RTG][RECOVER][OFFLINE_RELEASE]`
- `[RTG][DISPATCH][POST-ACQUIRE]`
- `[RTG][ACQUIRE][RESULT]`

If further stall signatures remain after this pass, the next likely inspection surface is not planner math first, but **lane starvation fairness vs. repeated helper reuse pressure across simultaneous BG families**.
