#include "DungeonAssistTankTrigger.h"
#include "Playerbots.h"

bool DungeonAssistTankTrigger::IsActive()
{
    Map* map = bot->GetMap();
    if (!map)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // In normal cases this is dungeon-only. If you want it ONLY in dungeons, keep IsDungeon().
    // If you want it to also work during brief LFG transitions outside the instance, allow LFG group too.
    if (!map->IsDungeon() && !group->isLFGGroup())
        return false;

    // If we already have a victim, combat logic takes over; don't spam assist.
    if (bot->GetVictim())
        return false;

    // If tank published a pull target, assist immediately
    ObjectGuid pull = AI_VALUE(ObjectGuid, "pull target");
    if (pull)
        return true;

    // If tank already has a target, assist immediately
    Unit* tankTarget = AI_VALUE(Unit*, "tank target");
    if (tankTarget && tankTarget->IsAlive())
        return true;

    // If ANYONE in party is already in combat, assist immediately
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (m && m->IsInCombat())
            return true;
    }

    return false;
}