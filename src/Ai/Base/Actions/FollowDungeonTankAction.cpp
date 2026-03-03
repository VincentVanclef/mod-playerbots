#include "FollowDungeonTankAction.h"

#include "Group.h"
#include "MotionMaster.h"
#include "Playerbots.h"

Player* FollowDungeonTankAction::GetGroupTank() const
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    // Prefer actual tanks (playerbot role helper)
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (!m || !m->IsInWorld())
            continue;

        if (botAI->IsTank(m))
            return m;
    }

    // Fallback: group leader if no tank detected
    if (Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()))
        return leader;

    return nullptr;
}

bool FollowDungeonTankAction::Execute(Event /*event*/)
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // Tank should not follow anyone (tank leads)
    if (botAI->IsTank(bot))
        return false;

    Player* tank = GetGroupTank();
    if (!tank || tank == bot || !tank->IsAlive())
        return false;

    // If tank isn't actually in the dungeon, don't chase endlessly
    Map* tankMap = tank->GetMap();
    if (!tankMap || !tankMap->IsDungeon())
        return false;

    float dist = botAI->IsHeal(bot) ? 16.0f : 10.0f; // healer stays farther back
    float maxCatchup = 60.0f;                        // if tank is insanely far, don't try to path forever

    if (!bot->IsWithinDistInMap(tank, maxCatchup))
        return false;

    // If we're already close enough, do nothing
    if (bot->IsWithinDistInMap(tank, dist))
        return false;

    // Follow the tank (does not change "master", just movement)
    bot->GetMotionMaster()->MoveFollow(tank, dist, 0.0f);
    return true;
}