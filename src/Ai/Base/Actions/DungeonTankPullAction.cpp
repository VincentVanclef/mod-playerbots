#include "DungeonTankPullAction.h"

#include "Playerbots.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "GameTime.h"

#include <unordered_map>

namespace
{
    // botId -> unix time seconds when we first noticed "no pull target"
    static std::unordered_map<uint32, uint32> sTankNoTargetSince;
}

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

        // Acquire targets a bit farther away so the tank can start walking to the next pack
        if (!bot->IsWithinDistInMap(u, 45.0f)) // tune
            continue;

        if (!bot->IsWithinLOSInMap(u))
            continue;

        // Don't pull targets already fighting something
        if (u->IsInCombat())
            continue;

        // Avoid targets that are already in messy combat clusters
        if (u->GetAttackers().size() >= 3)
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

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 now = GameTime::GetGameTime();

    Unit* target = PickPullTarget();
    if (!target)
    {
        // Compromise: don't "hard follow" all the time, but don't get lost either.
        // Start a timer; if we've had no target for 10s, regroup near leader.
        uint32& since = sTankNoTargetSince[botId];
        if (!since)
            since = now;

        if (now >= since + 10)
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            if (leader && leader != bot && leader->IsInWorld())
            {
                if (!bot->IsWithinDistInMap(leader, 25.0f))
                {
                    bot->GetMotionMaster()->MoveFollow(leader, 18.0f, 0.0f);
                    return true;
                }
            }
        }

        return false;
    }

    // We found a target again: clear the no-target timer
    sTankNoTargetSince.erase(botId);

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