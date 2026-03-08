# RTG Role/Context Framework Roadmap

This test zip lays groundwork for a broader role-aware playerbot decision framework.

## Goals
- Make RDF role choice match actual spec.
- Prevent BG support from being starved by RDF support.
- Push healer specs toward healing-first behavior in dungeon/LFG context.
- Keep class flavor alive by treating survival tools as desperation fallback instead of removing them entirely.

## What this zip changes
1. **Spec-truth role detection**
   - `RandomPlayerbotMgr.cpp` now uses `AiFactory::GetPlayerSpecTab(bot)` and the playerbot spec-tab enums for RDF role truth.
2. **RDF/BG fairness groundwork**
   - When both queues need bots, the helper budget now alternates if only one slot is available and guarantees at least one share each when two or more slots are available.
3. **Dungeon healer suppression / support bias**
   - `AttackAction.cpp` suppresses healer offensive pull behavior more aggressively in dungeon/LFG runs.
   - `NonCombatActions.cpp` delays healer drinking when the group is fighting or has moved too far ahead.
4. **Healer strategy tuning**
   - Priest and Paladin healer defaults no longer lead with offensive filler.
   - Druid and Shaman healer defaults lean further into proactive healing and tank support.
5. **Emergency-only druid bear fallback**
   - Resto-style healers in dungeon/LFG context can still bear-form as a last resort if they are personally being hit and are in critical health, instead of freely drifting into tank behavior.

## Recommended next pass
- Add a shared `role/context` score modifier layer that adjusts action relevance globally for tank/healer/dps roles.
- Add BG-specific role/context weighting so healers still heal aggressively in battleground support.
- Add tank protection priorities: peel healer, pick up loose adds, value taunt/defensives above raw DPS.
- Add DPS target policy: focus current kill target, avoid threat spikes, use interrupts/utility.
- Add vote-to-kick handling for dead/DC/not-in-map bots after queue behavior is stable.
