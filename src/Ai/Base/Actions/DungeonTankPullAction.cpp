#include "DungeonTankPullAction.h"

#include "Playerbots.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"

bool DungeonTankPullAction::PartyHealerReady(uint8 minManaPct) const
{
    Group* group = bot->GetGroup();
    if (!group)
        return true; // solo tank: go ahead

    // Find a healer in group and check mana%
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;

        if (!botAI->IsHeal(member))
            continue;

        // If healer has no mana bar (rage/energy user etc) treat as "ready"
        if (member->GetMaxPower(POWER_MANA) == 0)
            return true;

        uint8 manaPct = uint8((float(member->GetPower(POWER_MANA)) / float(member->GetMaxPower(POWER_MANA))) * 100.0f);
        return manaPct >= minManaPct;
    }

    // No healer found => allow pull
    return true;
}

Unit* DungeonTankPullAction::PickPullTarget() const
{
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (ObjectGuid const& g : targets)
    {
        Unit* u = botAI->GetUnit(g);
        if (!u || !u->IsInWorld() || u->isDead())
            continue;

        if (bot->IsFriendlyTo(u))
            continue;

        if (u->GetCreatureType() == CREATURE_TYPE_CRITTER)
            continue;

        // Keep pulls tight so tank doesn't sprint through packs
        if (!bot->IsWithinDistInMap(u, 45.0f)) // tune
            continue;

        if (!bot->IsWithinLOSInMap(u))
            continue;

        // Don't pull targets already fighting something
        if (u->IsInCombat())
            continue;

        // Avoid targets that are already in messy combat clusters
        if (u->getAttackers().size() >= 3)
            continue;

        return u;
    }

    return nullptr;
}

bool DungeonTankPullAction::Execute(Event /*event*/)
{
    // Only tanks should run this
    if (!botAI->IsTank(bot))
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    if (bot->IsInCombat())
        return false;

    if (bot->IsMounted() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsBeingTeleported())
        return false;

    // If party healer isn’t ready, don’t pull
    if (!PartyHealerReady(70))
        return false;

    // If anyone in group is still in combat, do NOT pull (prevents chain pulls)
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (m && m->IsInCombat())
            return false;
    }

    Unit* target = PickPullTarget();
	if (!target)
	{
		// No safe pull target right now.
		// Compromise: regroup near the party so the tank doesn't get lost or left behind.
		Group* group = bot->GetGroup();
		if (!group)
			return false;

		Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
		if (!leader || leader == bot)
			return false;

		// If we're far from the party, move back in (but don't "follow forever")
		if (!bot->IsWithinDistInMap(leader, 25.0f))
		{
			bot->GetMotionMaster()->MoveFollow(leader, 18.0f, 0.0f);
			return true;
		}

		return false;
	}

	// If target is not in pull range yet, move into position (don’t wait for player body-pull)
	if (!bot->IsWithinDistInMap(target, 25.0f))
	{
		// Move into line-of-sight like a ranged pull setup (safer than charging through packs)
		return MoveToLOS(target, true); // ranged = true
	}

	// Now we’re in pull range: publish pull target so DPS/heals instantly assist
	context->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());
	botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({ target->GetGUID() });

	return Attack(target);
}