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
