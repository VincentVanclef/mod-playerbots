/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "NonCombatActions.h"

#include "Event.h"
#include "MotionMaster.h"
#include "Playerbots.h"

namespace
{
constexpr uint32 BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 BG_EY_NETHERSTORM_FLAG_SPELL = 34976;

bool IsDisallowedShapeshiftForm(Player* bot)
{
    if (bot->getClass() == CLASS_DRUID)
    {
        ShapeshiftForm form = bot->GetShapeshiftForm();
        return form == FORM_TRAVEL || form == FORM_AQUA ||
               form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ||
               form == FORM_BEAR || form == FORM_DIREBEAR ||
               form == FORM_CAT;
    }
    else if (bot->getClass() == CLASS_PRIEST)
    {
        return bot->GetShapeshiftForm() == FORM_SPIRITOFREDEMPTION;
    }

    return false;
}

bool HasRealAttackers(PlayerbotAI* botAI)
{
    return !botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get().empty();
}

void PrepareForRest(Player* bot, PlayerbotAI* botAI)
{
    bot->AttackStop();
    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);

    if (bot->isMoving())
        bot->StopMoving();

    // A follow/chase movement generator can restart movement without another AI
    // decision. Clear it so the food or drink aura is not cancelled immediately.
    if (MotionMaster* motion = bot->GetMotionMaster())
        motion->Clear();

    botAI->InterruptSpell();
    bot->SetStandState(UNIT_STAND_STATE_SIT);
}
}

bool DrinkAction::Execute(Event event)
{
    if (bot->IsInCombat() && !HasRealAttackers(botAI))
        bot->CombatStop(true);

    if (botAI->HasCheat(BotCheatMask::food))
    {
        // if (bot->IsNonMeleeSpellCast(true))
        //     return false;

        PrepareForRest(bot, botAI);

        // float hp = bot->GetHealthPercent();
        float mp = bot->GetPowerPct(POWER_MANA);
        float p = mp;
        float delay;

        if (!bot->InBattleground())
            delay = 18000.0f * (100 - p) / 100.0f;
        else
            delay = 12000.0f * (100 - p) / 100.0f;

        botAI->SetNextCheckDelay(std::max(3000.0f, delay));

        bot->AddAura(25990, bot);
        return true;
        // return botAI->CastSpell(24707, bot);
    }

    return UseItemAction::Execute(event);
}

bool DrinkAction::isUseful()
{
    return UseItemAction::isUseful() && AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < 100;
}

bool DrinkAction::isPossible()
{
    if (bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    // Dungeon combat can remain latched briefly after the pull is dead. Treat
    // that state as safe only when the AI cannot find any living hostile unit
    // threatening the bot or its nearby group.
    if (bot->IsInCombat() && HasRealAttackers(botAI))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return botAI->HasCheat(BotCheatMask::food) || UseItemAction::isPossible();
}

bool EatAction::Execute(Event event)
{
    if (bot->IsInCombat() && !HasRealAttackers(botAI))
        bot->CombatStop(true);

    if (botAI->HasCheat(BotCheatMask::food))
    {
        // if (bot->IsNonMeleeSpellCast(true))
        //     return false;

        PrepareForRest(bot, botAI);

        float hp = bot->GetHealthPct();
        // float mp = bot->HasMana() ? bot->GetPowerPercent() : 0.f;
        float p = hp;
        float delay;

        if (!bot->InBattleground())
            delay = 18000.0f * (100 - p) / 100.0f;
        else
            delay = 12000.0f * (100 - p) / 100.0f;

        botAI->SetNextCheckDelay(std::max(3000.0f, delay));

        bot->AddAura(25990, bot);
        return true;
    }

    return UseItemAction::Execute(event);
}

bool EatAction::isUseful() { return UseItemAction::isUseful() && AI_VALUE2(uint8, "health", "self target") < 100; }

bool EatAction::isPossible()
{
    if (bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    if (bot->IsInCombat() && HasRealAttackers(botAI))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return botAI->HasCheat(BotCheatMask::food) || UseItemAction::isPossible();
}
