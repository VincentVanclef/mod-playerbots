# RTG BG assistance refactor

## Goal
Split RTG battleground helper behavior into two layers:

1. **Demand layer** — decide whether new helper bots are still needed.
2. **Lifecycle safety layer** — decide whether an existing helper is still owned by live battleground state and therefore must not be retired yet.

## Files changed
- `src/Bot/RandomPlayerbotMgr.h`
- `src/Bot/RandomPlayerbotMgr.cpp`
- `src/Ai/Base/Actions/BattleGroundJoinAction.cpp`

## What changed

### 1) BattlegroundInfo now carries split RTG counters
Added queue-vs-active battleground counters so the manager can reason about:
- queued real players / bots
- active in-instance real players / bots

Legacy aggregate counters are still preserved for existing join logic.

### 2) CheckBgQueue now records battleground state with a safer split model
Refactored BG accounting so the pass now:
- considers both `InBattlegroundQueue()` and `InBattleground()`
- records queue state separately from active instance state
- preserves legacy aggregate counts with per-participant deduplication
- keeps active instance IDs tracked for lifecycle-aware decisions

### 3) BG helper cleanup now honors lifecycle ownership first
Refactored helper retirement so:
- if a helper is still owned by battleground lifecycle, it is never force-retired
- helpers are only marked with `rtg_bg_retire_when_safe`
- retirement only happens once the helper is fully detached from BG queue / invite / instance / BG-group state
- helpers that are still needed are kept reserved instead of being aggressively recycled while idle

### 4) BG group teardown remains protected
The earlier surgery to avoid force-disbanding live BG groups remains aligned with this refactor.

### 5) BG leave path is safer in multi-queue situations
`BGLeaveAction` no longer hard-assumes queue slot `0`; it now searches available BG queue slots first and falls back to the RTG assignment metadata.

### 6) Small cleanup in BGJoinAction
Removed duplicated `assignedHelper` allowance logic.

## Why this should help your repro
This is designed to prevent the bad chain where:
- AB demand drops
- the helper manager thinks helpers are stale
- bots still tied to live BG state get torn down too early
- the battleground instance is left in a partial or inconsistent state

Now the queue-demand layer can say **"we do not need more bots"** without the lifecycle layer incorrectly saying **"rip out bots that are still owned by a live battleground."**

## What I would test first
1. Queue AB and WSG together.
2. Let helpers log in and join.
3. Leave AB.
4. Re-enter if AB remains open.
5. Watch whether helpers remain stable and only retire after fully leaving BG-owned state.

## Next likely follow-up if needed
If issues remain after this pass, the next place to harden is:
- helper ownership persistence per battleground instance
- explicit RTG helper state markers like assigned / queued / invited / active / retire_when_safe
