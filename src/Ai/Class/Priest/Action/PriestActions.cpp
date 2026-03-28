/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PriestActions.h"

#include "Event.h"
#include "Playerbots.h"

bool CastRemoveShadowformAction::isUseful() { return botAI->HasAura("shadowform", AI_VALUE(Unit*, "self target")); }

bool CastRemoveShadowformAction::isPossible() { return true; }

bool CastRemoveShadowformAction::Execute(Event event)
{
    botAI->RemoveAura("shadowform");
    return true;
}

Unit* CastPowerWordShieldOnAlmostFullHealthBelowAction::GetTarget()
{
    if (Unit* mainTank = AI_VALUE(Unit*, "main tank"))
    {
        if (!mainTank->isDead() && mainTank->GetHealthPct() <= sPlayerbotAIConfig.almostFullHealth &&
            mainTank->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
            !botAI->HasAnyAuraOf(mainTank, "weakened soul", "power word: shield", nullptr))
            return mainTank;
    }

    Group* group = bot->GetGroup();
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.almostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.spellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return player;
    }
    return nullptr;
}

bool CastPowerWordShieldOnAlmostFullHealthBelowAction::isUseful()
{
    if (Unit* mainTank = AI_VALUE(Unit*, "main tank"))
    {
        if (!mainTank->isDead() && mainTank->GetHealthPct() <= sPlayerbotAIConfig.almostFullHealth &&
            mainTank->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
            !botAI->HasAnyAuraOf(mainTank, "weakened soul", "power word: shield", nullptr))
            return true;
    }

    Group* group = bot->GetGroup();
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead())
        {
            continue;
        }
        if (player->GetHealthPct() > sPlayerbotAIConfig.almostFullHealth)
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.spellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        return true;
    }
    return false;
}

Unit* CastPowerWordShieldOnNotFullAction::GetTarget()
{
    if (Unit* mainTank = AI_VALUE(Unit*, "main tank"))
    {
        if (!mainTank->isDead() && !mainTank->IsFullHealth() &&
            mainTank->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
            !botAI->HasAnyAuraOf(mainTank, "weakened soul", "power word: shield", nullptr))
            return mainTank;
    }

    Group* group = bot->GetGroup();
    MinValueCalculator calc(100);
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player)
            continue;
        if (player->isDead() || player->IsFullHealth())
        {
            continue;
        }
        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.spellDistance)
        {
            continue;
        }
        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
        {
            continue;
        }
        calc.probe(player->GetHealthPct(), player);
    }
    return (Unit*)calc.param;
}

bool CastPowerWordShieldOnNotFullAction::isUseful()
{
    return GetTarget();
}
