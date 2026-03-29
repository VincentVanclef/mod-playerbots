/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "HealPriestStrategy.h"

#include "GenericPriestStrategyActionNodeFactory.h"
#include "Playerbots.h"

HealPriestStrategy::HealPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    actionNodeFactories.Add(new GenericPriestStrategyActionNodeFactory());
}

std::vector<NextAction> HealPriestStrategy::getDefaultActions()
{
    return {
        NextAction("power word: shield on not full", ACTION_DEFAULT + 4.0f),
        NextAction("power word: shield on almost full health below", ACTION_DEFAULT + 3.5f),
        NextAction("renew on party", ACTION_DEFAULT + 0.5f),
        NextAction("power word: shield on party", ACTION_DEFAULT)
    };
}

void HealPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(
            "group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 8),
                NextAction("power word: shield on not full", ACTION_MEDIUM_HEAL + 12),
                NextAction("power word: shield on almost full health below", ACTION_MEDIUM_HEAL + 11),
                NextAction("renew on party", ACTION_MEDIUM_HEAL + 6)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("divine hymn", ACTION_CRITICAL_HEAL + 7),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("power word: shield on not full", ACTION_CRITICAL_HEAL + 9),
                NextAction("power word: shield on almost full health below", ACTION_CRITICAL_HEAL + 8),
                NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 4)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member critical health",
            {
                NextAction("power word: shield on not full", ACTION_CRITICAL_HEAL + 10),
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 9),
                NextAction("penance on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 5),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 3)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("power word: shield on not full", ACTION_MEDIUM_HEAL + 8),
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 7),
                NextAction("penance on party", ACTION_MEDIUM_HEAL + 5),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 2)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                NextAction("power word: shield on not full", ACTION_LIGHT_HEAL + 13),
                NextAction("power word: shield on party", ACTION_LIGHT_HEAL + 12),
                NextAction("penance on party", ACTION_LIGHT_HEAL + 9),
                NextAction("flash heal on party", ACTION_LIGHT_HEAL + 8),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 6)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member almost full health",
            {
                NextAction("power word: shield on almost full health below", ACTION_LIGHT_HEAL + 6),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 2),
                NextAction("renew on party", ACTION_LIGHT_HEAL + 1)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "critical health", {
                NextAction("pain suppression", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "protect party member",
            {
                NextAction("pain suppression on party", ACTION_EMERGENCY)
            }
        )
    );
}
