#ifndef _PLAYERBOT_DUNGEONTANKPULLACTION_H
#define _PLAYERBOT_DUNGEONTANKPULLACTION_H

#include "AttackAction.h"

class DungeonTankPullAction : public AttackAction
{
public:
    DungeonTankPullAction(PlayerbotAI* botAI) : AttackAction(botAI, "dungeon tank pull") {}

    bool Execute(Event event) override;

private:
    bool PartyHealerReady(uint8 minManaPct) const;
    Unit* PickPullTarget() const;
};

#endif