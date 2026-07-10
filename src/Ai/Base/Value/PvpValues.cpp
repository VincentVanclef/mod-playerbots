/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PvpValues.h"

#include "BattlegroundEY.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "Playerbots.h"
#include "RTGBattlegroundObjectiveBrain.h"
#include "ServerFacade.h"

namespace
{
TeamId RTG_ValueEffectiveBgTeamId(Player* player)
{
    return RTG_GetEffectiveBgTeam(player);
}
}

Unit* FlagCarrierValue::Calculate()
{
    Unit* carrier = nullptr;

    if (botAI->GetBot()->InBattleground())
    {
        if (botAI->GetBot()->GetBattlegroundTypeId() == BattlegroundTypeId::BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();

            if (!bg)
                return nullptr;

            TeamId const team = RTG_ValueEffectiveBgTeamId(bot);
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                return nullptr;

            // In WSG, GetFlagPickerGUID(team) returns the carrier of that team's own flag.
            // For "enemy flag carrier", chase whoever has our flag. For "team flag carrier",
            // support whoever has the enemy flag. Use BG team, not original faction, so CFBG
            // and fake-faction states do not invert Alliance/Horde objective logic.
            TeamId const flagOwnerTeam = sameTeam ? bg->GetOtherTeamId(team) : team;
            if (!bg->GetFlagPickerGUID(flagOwnerTeam).IsEmpty())
                carrier = ObjectAccessor::GetPlayer(bg->GetBgMap(), bg->GetFlagPickerGUID(flagOwnerTeam));

            if (carrier)
            {
                if (ignoreRange || bot->IsWithinDistInMap(carrier, sPlayerbotAIConfig.sightDistance))
                {
                    return carrier;
                }
                else
                    return nullptr;
            }
        }

        if (botAI->GetBot()->GetBattlegroundTypeId() == BATTLEGROUND_EY)
        {
            BattlegroundEY* bg = (BattlegroundEY*)botAI->GetBot()->GetBattleground();

            if (!bg)
                return nullptr;

            if (bg->GetFlagPickerGUID().IsEmpty())
                return nullptr;

            Player* fc = ObjectAccessor::GetPlayer(bg->GetBgMap(), bg->GetFlagPickerGUID());
            if (!fc)
                return nullptr;

            TeamId const botTeam = RTG_ValueEffectiveBgTeamId(bot);
            TeamId const fcTeam = RTG_ValueEffectiveBgTeamId(fc);

            if (!sameTeam && fcTeam != TEAM_NEUTRAL && botTeam != TEAM_NEUTRAL && fcTeam != botTeam)
                carrier = fc;

            if (sameTeam && fcTeam != TEAM_NEUTRAL && botTeam != TEAM_NEUTRAL && fcTeam == botTeam)
                carrier = fc;

            if (carrier)
            {
                if (ignoreRange || bot->IsWithinDistInMap(carrier, sPlayerbotAIConfig.sightDistance))
                {
                    return carrier;
                }
                else
                    return nullptr;
            }
        }
    }

    return carrier;
}

std::vector<CreatureData const*> BgMastersValue::Calculate()
{
    BattlegroundTypeId bgTypeId = (BattlegroundTypeId)stoi(qualifier);

    std::vector<uint32> entries;
    std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> battleMastersCache =
        sRandomPlayerbotMgr.getBattleMastersCache();
    entries.insert(entries.end(), battleMastersCache[TEAM_NEUTRAL][bgTypeId].begin(),
                   battleMastersCache[TEAM_NEUTRAL][bgTypeId].end());
    entries.insert(entries.end(), battleMastersCache[TEAM_ALLIANCE][bgTypeId].begin(),
                   battleMastersCache[TEAM_ALLIANCE][bgTypeId].end());
    entries.insert(entries.end(), battleMastersCache[TEAM_HORDE][bgTypeId].begin(),
                   battleMastersCache[TEAM_HORDE][bgTypeId].end());

    std::vector<CreatureData const*> bmGuids;

    for (auto entry : entries)
    {
        for (auto creaturePair : WorldPosition().getCreaturesNear(0, entry))
        {
            bmGuids.push_back(creaturePair);
        }
    }

    return bmGuids;
}

CreatureData const* BgMasterValue::Calculate()
{
    CreatureData const* bmPair = NearestBm(false);
    if (!bmPair)
        bmPair = NearestBm(true);

    return bmPair;
}

CreatureData const* BgMasterValue::NearestBm(bool allowDead)
{
    WorldPosition botPos(bot);

    std::vector<CreatureData const*> bmPairs = AI_VALUE2(std::vector<CreatureData const*>, "bg masters", qualifier);

    float rDist = 0.0f;
    CreatureData const* rbmPair = nullptr;

    for (auto& bmPair : bmPairs)
    {
        if (!bmPair)
            continue;

        WorldPosition bmPos(bmPair->mapid, bmPair->posX, bmPair->posY, bmPair->posZ, bmPair->orientation);

        float dist = botPos.distance(bmPos);  // This is the aproximate travel distance.

        // Did we already find a closer unit that is not dead?
        if (rbmPair && rDist <= dist)
            continue;

        CreatureTemplate const* bmTemplate = sObjectMgr->GetCreatureTemplate(bmPair->id1);
        if (!bmTemplate)
            continue;

        FactionTemplateEntry const* bmFactionEntry = sFactionTemplateStore.LookupEntry(bmTemplate->faction);

        // Is the unit hostile?
        if (Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), bmFactionEntry) < REP_NEUTRAL)
            continue;

        AreaTableEntry const* area = bmPos.getArea();

        if (!area)
            continue;

        // Is the area hostile?
        if (area->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;
        if (area->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (!allowDead)
        {
            Unit* unit = botAI->GetUnit(bmPair);

            if (!unit)
                continue;

            // Is the unit dead?
            if (unit->getDeathState() == DeathState::Dead)
                continue;
        }

        rbmPair = bmPair;
        rDist = dist;
    }

    return rbmPair;
}

BattlegroundTypeId RpgBgTypeValue::Calculate()
{
    GuidPosition guidPosition = AI_VALUE(GuidPosition, "rpg target");

    if (guidPosition)
        for (uint32 i = 1; i < MAX_BATTLEGROUND_QUEUE_TYPES; i++)
        {
            BattlegroundQueueTypeId queueTypeId = (BattlegroundQueueTypeId)i;

            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);

            Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            if (bot->GetLevel() < bg->GetMinLevel())
                continue;

            // check if already in queue
            if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
                continue;

            std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> battleMastersCache =
                sRandomPlayerbotMgr.getBattleMastersCache();

            for (auto& entry : battleMastersCache[TEAM_NEUTRAL][bgTypeId])
                if (entry == guidPosition.GetEntry())
                    return bgTypeId;

            for (auto& entry : battleMastersCache[bot->GetTeamId()][bgTypeId])
                if (entry == guidPosition.GetEntry())
                    return bgTypeId;
        }

    return BATTLEGROUND_TYPE_NONE;
}
