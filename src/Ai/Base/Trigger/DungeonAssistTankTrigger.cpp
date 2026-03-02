#include "DungeonAssistTankTrigger.h"
#include "Playerbots.h"

bool DungeonAssistTankTrigger::IsActive()
{
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    if (!bot->GetGroup())
        return false;

    if (bot->IsInCombat())
        return false;

    ObjectGuid pull = AI_VALUE(ObjectGuid, "pull target");
    if (pull)
        return true;

    return false;
}