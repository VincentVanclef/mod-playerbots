/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AttackAction.h"

#include "CreatureAI.h"
#include "Event.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Unit.h"

namespace
{
static bool RTG_IsPassiveDungeonHealer(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI)
        return false;

    if (!botAI->IsHeal(bot))
        return false;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    return (group && group->isLFGGroup()) || (map && map->IsDungeon());
}

static bool RTG_GroupNeedsUrgentHealing(Player* bot)
{
    if (!bot)
        return false;

    if (bot->GetHealthPct() < 95.0f)
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->isDead())
            continue;

        if (member->GetHealthPct() < 95.0f)
            return true;
    }

    return false;
}
}

bool AttackAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!target->IsInWorld())
        return false;

    if (RTG_IsPassiveDungeonHealer(bot, botAI))
    {
        // Healers should not engage unless directly attacked. Keep them focused on heals.
        if (target->GetVictim() != bot || RTG_GroupNeedsUrgentHealing(bot))
        {
            bot->AttackStop();
            bot->SetTarget(ObjectGuid::Empty);
            bot->SetSelection(ObjectGuid::Empty);
            context->GetValue<Unit*>("current target")->Set(nullptr);
            return false;
        }
    }

    return Attack(target);
}

bool AttackMyTargetAction::Execute(Event /*event*/)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    ObjectGuid guid = master->GetTarget();
    if (!guid)
    {
        if (verbose)
            botAI->TellError("You have no target");

        return false;
    }

    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({guid});
    bool result = Attack(botAI->GetUnit(guid));
    if (result)
        context->GetValue<ObjectGuid>("pull target")->Set(guid);

    return result;
}

bool AttackAction::Attack(Unit* target, bool /*with_pet*/ /*true*/)
{
    Unit* oldTarget = context->GetValue<Unit*>("current target")->Get();
    bool shouldMelee = bot->IsWithinMeleeRange(target) || botAI->IsMelee(bot);
    
    // Range close (melee/ranged): ensure we actually step into engagement distance instead of stalling
    bool isRanged = !shouldMelee;


    bool sameTarget = oldTarget == target && bot->GetVictim() == target;
    bool inCombat = botAI->GetState() == BOT_STATE_COMBAT;
    bool sameAttackMode = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == shouldMelee;

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        if (verbose)
            botAI->TellError("I cannot attack in flight");

        return false;
    }

    if (!target)
    {
        if (verbose)
            botAI->TellError("I have no target");

        return false;
    }

    if (!target->IsInWorld())
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is no longer in the world.");

        return false;
    }

    if (RTG_IsPassiveDungeonHealer(bot, botAI))
    {
        // Strict healer rule for dungeon/LFG bots: do not initiate combat unless being hit directly.
        if (target->GetVictim() != bot || RTG_GroupNeedsUrgentHealing(bot))
            return false;
    }

    // Check if bot OR target is in prohibited zone/area (skip for duels)
    if ((target->IsPlayer() || target->IsPet()) &&
        (!bot->duel || bot->duel->Opponent != target) &&
        (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(target->GetZoneId(), target->GetAreaId())))
    {
        if (verbose)
            botAI->TellError("I cannot attack other players in PvP prohibited areas.");

        return false;
    }

    if (bot->IsFriendlyTo(target))
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is friendly to me.");

        return false;
    }

    if (target->isDead())
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is dead.");

        return false;
    }

    if (!bot->IsWithinLOSInMap(target))
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is not in my sight.");

        return false;
    }

    
    // Ensure we close to appropriate distance for engagement.
    // Without this, bots can target something indefinitely while staying just out of range.
    if (botAI->CanMove())
    {
        if (!isRanged)
        {
            if (!bot->IsWithinMeleeRange(target))
            {
                ReachCombatTo(target, target->GetCombatReach());
                return false;
            }
        }
        else
        {
            float engageDist = sPlayerbotAIConfig.spellDistance;
            if (engageDist < 10.0f) engageDist = 10.0f;
            if (bot->GetExactDist2d(target) > engageDist)
            {
                ReachCombatTo(target, engageDist);
                return false;
            }
        }
    }
if (sameTarget && inCombat && sameAttackMode)
    {
        if (verbose)
            botAI->TellError("I am already attacking " + std::string(target->GetName()) + ".");

        return false;
    }

    if (!bot->IsValidAttackTarget(target))
    {
        if (verbose)
            botAI->TellError("I cannot attack an invalid target.");

        return false;
    }

    // if (bot->IsMounted() && bot->IsWithinLOSInMap(target))
    // {
    //     WorldPacket emptyPacket;
    //     bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    // }

    ObjectGuid guid = target->GetGUID();
    bot->SetSelection(target->GetGUID());

        context->GetValue<Unit*>("old target")->Set(oldTarget);

    context->GetValue<Unit*>("current target")->Set(target);
    context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && bot->isMoving() && !moveControlled)
    {
        AI_VALUE(LastMovement&, "last movement").clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (botAI->CanMove() && !bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    botAI->ChangeEngine(BOT_STATE_COMBAT);

    bot->Attack(target, shouldMelee);
    /* prevent pet dead immediately in group */
    // if (bot->GetMap()->IsDungeon() && bot->GetGroup() && !target->IsInCombat())
    // {
    //     with_pet = false;
    // }
    // if (Pet* pet = bot->GetPet())
    // {
    //     if (with_pet)
    //     {
    //         pet->SetReactState(REACT_DEFENSIVE);
    //         pet->SetTarget(target->GetGUID());
    //         pet->GetCharmInfo()->SetIsCommandAttack(true);
    //         pet->AI()->AttackStart(target);
    //     }
    //     else
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->GetCharmInfo()->IsReturning();
    //     }
    // }
    return true;
}

bool AttackDuelOpponentAction::isUseful() { return AI_VALUE(Unit*, "duel target"); }

bool AttackDuelOpponentAction::Execute(Event /*event*/) { return Attack(AI_VALUE(Unit*, "duel target")); }
