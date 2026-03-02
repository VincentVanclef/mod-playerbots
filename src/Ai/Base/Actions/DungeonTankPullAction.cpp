#include "DungeonTankPullAction.h"

#include "Playerbots.h"
#include "Group.h"
#include "ObjectAccessor.h"

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
    // Prefer targets that are actually in LOS and “possible”
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (ObjectGuid const& g : targets)
    {
        Unit* u = botAI->GetUnit(g);
        if (!u || !u->IsInWorld() || u->isDead())
            continue;

        if (bot->IsFriendlyTo(u))
            continue;

        // Don’t pull critters / nonsense
        if (u->GetCreatureType() == CREATURE_TYPE_CRITTER)
            continue;

        // Keep it close-ish so tank doesn’t sprint across the instance
        if (!bot->IsWithinDistInMap(u, sPlayerbotAIConfig.sightDistance))
            continue;

        if (!bot->IsWithinLOSInMap(u))
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

    if (!bot->GetGroup())
        return false;

    if (bot->IsInCombat())
        return false;

    if (bot->IsMounted() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsBeingTeleported())
        return false;

    // If party healer isn’t ready, don’t pull
    if (!PartyHealerReady(70))
        return false;

    // Don’t pull while anyone is drinking/eating nearby? (optional)
    // You can expand this later using party aura checks.

    Unit* target = PickPullTarget();
    if (!target)
        return false;

    // Mark pull target so other bots have something consistent to assist onto
    context->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());
    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({ target->GetGUID() });

    // Initiate combat
    return Attack(target);
}