#ifndef _PLAYERBOT_FOLLOWDUNGEONTANKACTION_H
#define _PLAYERBOT_FOLLOWDUNGEONTANKACTION_H

#include "Action.h"

class FollowDungeonTankAction : public Action
{
public:
    FollowDungeonTankAction(PlayerbotAI* botAI) : Action(botAI, "follow dungeon tank") {}

    bool Execute(Event event) override;

private:
    Player* GetGroupTank() const;
};

#endif