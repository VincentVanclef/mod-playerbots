/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PriestActions.h"

#include "Event.h"
#include "Playerbots.h"

namespace
{
    static Unit* RTG_GetShieldPrioritySupportTarget(Player* bot, PlayerbotAI* botAI)
    {
        if (!bot || !botAI)
            return nullptr;

        if (Unit* master = botAI->GetAiObjectContext()->GetValue<Unit*>("master target")->Get())
        {
            if (!master->isDead() && master != bot && master->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
                !botAI->HasAnyAuraOf(master, "weakened soul", "power word: shield", nullptr))
                return master;
        }

        return nullptr;
    }
}

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

    if (Unit* supportTarget = RTG_GetShieldPrioritySupportTarget(bot, botAI))
    {
        if (supportTarget->GetHealthPct() <= sPlayerbotAIConfig.almostFullHealth)
            return supportTarget;
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

    if (Unit* supportTarget = RTG_GetShieldPrioritySupportTarget(bot, botAI))
    {
        if (supportTarget->GetHealthPct() <= sPlayerbotAIConfig.almostFullHealth)
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
        if (!mainTank->isDead() &&
            mainTank->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
            !botAI->HasAnyAuraOf(mainTank, "weakened soul", "power word: shield", nullptr))
            return mainTank;
    }

    if (Unit* supportTarget = RTG_GetShieldPrioritySupportTarget(bot, botAI))
        return supportTarget;

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
    if (Unit* mainTank = AI_VALUE(Unit*, "main tank"))
    {
        if (!mainTank->isDead() &&
            mainTank->GetDistance2d(bot) <= sPlayerbotAIConfig.spellDistance &&
            !botAI->HasAnyAuraOf(mainTank, "weakened soul", "power word: shield", nullptr))
            return true;
    }

    if (RTG_GetShieldPrioritySupportTarget(bot, botAI))
        return true;

    return GetTarget();
}
