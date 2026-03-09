# RTG Queue Supervisor Hotfix

## Files changed
- `src/Bot/RandomPlayerbotMgr.cpp`

## Main fixes
- Real-demand tracking for BG buckets now survives active matches, not just queued states.
- Real-demand tracking for RDF/LFG now includes active dungeon groups, not just queued players.
- BG/LFG helper cleanup now uses real-demand checks before preserving bots.
- Bot-only non-LFG/non-real-player group preservation was tightened.
- Forced Dungeon Deserter casting during stale cleanup was removed.
- Cleanup now clears `Deserter` and `Dungeon Deserter` before logout and revives dead bots so they do not linger at GY.
- Active managed BG/LFG bots have queue deserter auras stripped during supervisor passes.

## Intent
This hotfix moves the module closer to RTG's rule set:
1. BG bots are valid only while the BG bucket still has real-player demand.
2. RDF bots are valid only while the RDF owner/group still has real-player demand or an active real-player group context.
3. Bot-only lingering world state should collapse faster.
4. Refill logic should work for active BG/RDF groups, not only fresh queues.
