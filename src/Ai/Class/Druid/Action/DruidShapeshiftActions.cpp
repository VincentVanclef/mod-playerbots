/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DruidShapeshiftActions.h"

#include "Playerbots.h"

namespace
{
    static bool RTG_AllowHealerBearForm(PlayerbotAI* botAI)
    {
        Player* bot = botAI ? botAI->GetBot() : nullptr;
        if (!botAI || !bot || !botAI->IsHeal(bot, true))
            return true;

        Group* group = bot->GetGroup();
        Map* map = bot->GetMap();
        bool inDungeonRun = (group && group->isLFGGroup()) || (map && map->IsDungeon());
        if (!inDungeonRun)
            return true;

        Unit* victim = bot->GetVictim();
        if (!victim || victim->GetVictim() != bot)
            return false;

        return bot->GetHealthPct() <= 35.0f;
    }
}

bool CastBearFormAction::isPossible()
{
    return RTG_AllowHealerBearForm(botAI) && CastBuffSpellAction::isPossible() && !botAI->HasAura("dire bear form", GetTarget());
}

bool CastBearFormAction::isUseful()
{
    return RTG_AllowHealerBearForm(botAI) && CastBuffSpellAction::isUseful() && !botAI->HasAura("dire bear form", GetTarget());
}

std::vector<NextAction> CastDireBearFormAction::getAlternatives()
{
    return NextAction::merge({ NextAction("bear form") },
                             CastSpellAction::getAlternatives());
}

bool CastTravelFormAction::isUseful()
{
    bool firstmount = bot->GetLevel() >= 20;

    // useful if no mount or with wsg flag
    return !bot->IsMounted() && (!firstmount || (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))) &&
           !botAI->HasAura("dash", bot);
}

bool CastCasterFormAction::isUseful()
{
    return botAI->HasAnyAuraOf(GetTarget(), "dire bear form", "bear form", "cat form", "travel form", "aquatic form",
                               "flight form", "swift flight form", "moonkin form", nullptr) &&
           AI_VALUE2(uint8, "mana", "self target") > sPlayerbotAIConfig.mediumHealth;
}

bool CastCasterFormAction::Execute(Event event)
{
    botAI->RemoveShapeshift();
    return true;
}

bool CastCancelTreeFormAction::isUseful()
{
    return botAI->HasAura(33891, bot);
}

bool CastCancelTreeFormAction::Execute(Event event)
{
    botAI->RemoveAura("tree of life");
    return true;
}

bool CastTreeFormAction::isUseful()
{
    return GetTarget() && CastSpellAction::isUseful() && !botAI->HasAura(33891, bot);
}
