#ifndef _PLAYERBOT_HEALERHOLDDPSACTION_H
#define _PLAYERBOT_HEALERHOLDDPSACTION_H

#include "Action.h"

class HealerHoldDpsAction : public Action
{
public:
    HealerHoldDpsAction(PlayerbotAI* botAI) : Action(botAI, "healer hold dps") {}
    bool Execute(Event event) override;

private:
    bool PartyNeedsHealing(float hpThreshold) const;
};

#endif