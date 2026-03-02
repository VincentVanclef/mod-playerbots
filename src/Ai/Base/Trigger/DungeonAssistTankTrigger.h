#ifndef _PLAYERBOT_DUNGEONASSISTTANKTRIGGER_H
#define _PLAYERBOT_DUNGEONASSISTTANKTRIGGER_H

#include "Trigger.h"

class DungeonAssistTankTrigger : public Trigger
{
public:
    DungeonAssistTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon assist tank", 1) {}
    bool IsActive() override;
};

#endif