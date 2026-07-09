/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PvpTriggers.h"

#include "BattleGroundTactics.h"
#include "BattlegroundEY.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "BattlegroundAV.h"
#include "BattlegroundEY.h"

namespace
{
TeamId RTG_TriggerEffectiveBgTeamId(Player* bot)
{
    if (!bot)
        return TEAM_NEUTRAL;

    // CFBG / fake-faction modules can make GetTeamId() disagree with the
    // battleground side. For flag state, FC, EFC, and own-base decisions, use
    // the actual BG team slot first.
    if (bot->InBattleground())
    {
        TeamId const bgTeam = bot->GetBgTeamId();
        if (bgTeam == TEAM_ALLIANCE || bgTeam == TEAM_HORDE)
            return bgTeam;
    }

    TeamId const team = bot->GetTeamId();
    if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
        return team;

    return TEAM_NEUTRAL;
}
}

bool EnemyPlayerNear::IsActive() { return AI_VALUE(Unit*, "enemy player target"); }

bool PlayerHasNoFlag::IsActive()
{
    if (botAI->GetBot()->InBattleground())
    {
        if (botAI->GetBot()->GetBattlegroundTypeId() == BattlegroundTypeId::BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();
            TeamId const team = RTG_TriggerEffectiveBgTeamId(bot);
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                return true;

            if (!(bg->GetFlagState(bg->GetOtherTeamId(team)) == BG_WS_FLAG_STATE_ON_PLAYER))
                return true;

            if (bot->GetGUID() == bg->GetFlagPickerGUID(TEAM_ALLIANCE) ||
                bot->GetGUID() == bg->GetFlagPickerGUID(TEAM_HORDE))
            {
                return false;
            }
            return true;
        }
        return false;
    }

    return false;
}

bool PlayerIsInBattleground::IsActive() { return botAI->GetBot()->InBattleground(); }

bool BgWaitingTrigger::IsActive()
{
    if (bot->InBattleground())
    {
        if (bot->GetBattleground() && bot->GetBattleground()->GetStatus() == STATUS_WAIT_JOIN)
            return true;
    }

    return false;
}

bool BgActiveTrigger::IsActive()
{
    if (bot->InBattleground())
    {
        if (bot->GetBattleground() && bot->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS)
            return true;
    }

    return false;
}

bool BgInviteActiveTrigger::IsActive()
{
    if (bot->InBattleground() || !bot->InBattlegroundQueue())
    {
        return false;
    }

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(i);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);

        GroupQueueInfo ginfo;
        if (bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo))
        {
            if (ginfo.IsInvitedToBGInstanceGUID && ginfo.RemoveInviteTime)
            {
                LOG_INFO("playerbots", "Bot {} <{}> ({} {}) : Invited to BG but not in BG",
                         bot->GetGUID().ToString().c_str(), bot->GetName(), bot->GetLevel(),
                         bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H");
                return true;
            }
        }
    }

    return false;
}

bool InsideBGTrigger::IsActive() { return bot->InBattleground() && bot->GetBattleground(); }

bool PlayerIsInBattlegroundWithoutFlag::IsActive()
{
    if (botAI->GetBot()->InBattleground())
    {
        if (botAI->GetBot()->GetBattlegroundTypeId() == BattlegroundTypeId::BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();
            TeamId const team = RTG_TriggerEffectiveBgTeamId(bot);
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                return true;

            if (!(bg->GetFlagState(bg->GetOtherTeamId(team)) == BG_WS_FLAG_STATE_ON_PLAYER))
                return true;

            if (bot->GetGUID() == bg->GetFlagPickerGUID(TEAM_ALLIANCE) ||
                bot->GetGUID() == bg->GetFlagPickerGUID(TEAM_HORDE))
            {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool PlayerHasFlag::IsActive()
{
    return IsCapturingFlag(bot);
}

bool PlayerHasFlag::IsCapturingFlag(Player* bot)
{
    if (bot->InBattleground())
    {
        if (bot->GetBattlegroundTypeId() == BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)bot->GetBattleground();
            if (!bg)
                return false;

            TeamId const team = RTG_TriggerEffectiveBgTeamId(bot);
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                return false;

            TeamId const enemyTeam = bg->GetOtherTeamId(team);
            if (bot->GetGUID() != bg->GetFlagPickerGUID(enemyTeam))
                return false;

            // The bot has the enemy flag. If our own flag is also taken, only treat this
            // as a pure capture objective once the carrier has meaningfully left the
            // enemy flag room; otherwise allow local fighting/escape logic to run.
            if (!bg->GetFlagPickerGUID(team).IsEmpty())
            {
                uint32 const ownFlagObject = team == TEAM_HORDE ? BG_WS_OBJECT_H_FLAG : BG_WS_OBJECT_A_FLAG;
                if (GameObject* go = bg->GetBGObject(ownFlagObject))
                    return bot->GetDistance(go) > 36.0f;
            }

            return true;
        }

        if (bot->GetBattlegroundTypeId() == BATTLEGROUND_EY)
        {
            BattlegroundEY* bg = (BattlegroundEY*)bot->GetBattleground();

            // Check if bot has the flag
            if (bot->GetGUID() == bg->GetFlagPickerGUID())
            {
                // Count how many bases the bot's team owns
                uint32 controlledBases = 0;
                for (uint8 point = 0; point < EY_POINTS_MAX; ++point)
                {
                    if (bg->GetCapturePointInfo(point)._ownerTeamId == RTG_TriggerEffectiveBgTeamId(bot))
                        controlledBases++;
                }

                // If no bases are controlled, bot should go aggressive
                if (controlledBases == 0)
                    return false; // bot has flag but no place to take it

                // Otherwise, return false and stay defensive / move to base
                return bot->GetGUID() == bg->GetFlagPickerGUID();
            }
        }

        return false;
    }

    return false;
}

bool TeamHasFlag::IsActive()
{
    if (!botAI->GetBot()->InBattleground())
        return false;

    if (botAI->GetBot()->GetBattlegroundTypeId() != BattlegroundTypeId::BATTLEGROUND_WS)
        return false;

    BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();

    ObjectGuid botGuid = bot->GetGUID();
    TeamId teamId = RTG_TriggerEffectiveBgTeamId(bot);
    if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
        return false;

    TeamId enemyTeamId = bg->GetOtherTeamId(teamId);

    // If the bot is carrying any flag, don't activate
    if (botGuid == bg->GetFlagPickerGUID(TEAM_ALLIANCE) || botGuid == bg->GetFlagPickerGUID(TEAM_HORDE))
        return false;

    // Check: Own team has enemy flag, enemy team does NOT have your flag
    bool ownTeamHasFlag = bg->GetFlagState(enemyTeamId) == BG_WS_FLAG_STATE_ON_PLAYER;
    bool enemyTeamHasFlag = bg->GetFlagState(teamId) == BG_WS_FLAG_STATE_ON_PLAYER;

    return ownTeamHasFlag && !enemyTeamHasFlag;
}

bool EnemyTeamHasFlag::IsActive()
{
    if (botAI->GetBot()->InBattleground())
    {
        if (botAI->GetBot()->GetBattlegroundTypeId() == BattlegroundTypeId::BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();

            TeamId const team = RTG_TriggerEffectiveBgTeamId(bot);
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                return false;

            if (!bg->GetFlagPickerGUID(team).IsEmpty())
                return true;
        }

        return false;
    }

    return false;
}

bool EnemyFlagCarrierNear::IsActive()
{
    Unit* carrier = AI_VALUE(Unit*, "enemy flag carrier");

    if (!carrier || !ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, carrier), 100.f))
        return false;

    // Check if there is another enemy player target closer than the FC
    Unit* nearbyEnemy = AI_VALUE(Unit*, "enemy player target");

    if (nearbyEnemy)
    {
        float distToFC = ServerFacade::instance().GetDistance2d(bot, carrier);
        float distToEnemy = ServerFacade::instance().GetDistance2d(bot, nearbyEnemy);

        // If the other enemy is significantly closer, don't pursue FC
        if (distToEnemy + 15.0f < distToFC) // Add small buffer
            return false;
    }

    return true;
}

bool TeamFlagCarrierNear::IsActive()
{
    if (bot->GetBattlegroundTypeId() == BATTLEGROUND_WS)
    {
        BattlegroundWS* bg = dynamic_cast<BattlegroundWS*>(bot->GetBattleground());
        if (bg)
        {
            bool bothFlagsNotAtBase =
                bg->GetFlagState(TEAM_ALLIANCE) != BG_WS_FLAG_STATE_ON_BASE &&
                bg->GetFlagState(TEAM_HORDE) != BG_WS_FLAG_STATE_ON_BASE;

            if (bothFlagsNotAtBase)
                return false;
        }
    }

    Unit* carrier = AI_VALUE(Unit*, "team flag carrier");

    // RTG WSG: a 200y escort trigger made nearly the whole team peel onto the
    // friendly FC. Keep this trigger local; BGTactics::protectFC then restricts
    // actual escorting to a deterministic role.
    return carrier && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, carrier), 35.f);
}

bool PlayerWantsInBattlegroundTrigger::IsActive()
{
    if (bot->InBattleground())
        return false;

    if (bot->GetBattleground() && bot->GetBattleground()->GetStatus() == STATUS_WAIT_JOIN)
        return false;

    if (bot->GetBattleground() && bot->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS)
        return false;

    if (bot->IsDeserter())
        return false;

    return true;
}

bool VehicleNearTrigger::IsActive()
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest vehicles");
    return npcs.size();
}

bool InVehicleTrigger::IsActive() { return botAI->IsInVehicle(); }

bool AllianceNoSnowfallGY::IsActive()
{
    if (!bot || bot->GetTeamId() != TEAM_ALLIANCE)
        return false;

    Battleground* bg = bot->GetBattleground();
    if (bg && BGTactics::GetBotStrategyForTeam(bg, TEAM_ALLIANCE) != AV_STRATEGY_BALANCED)
        return false;

    float botX = bot->GetPositionX();
    if (botX <= -562.0f)
        return false;

    if (bot->GetBattlegroundTypeId() != BATTLEGROUND_AV)
        return false;

    if (BattlegroundAV* av = dynamic_cast<BattlegroundAV*>(bg))
    {
        const BG_AV_NodeInfo& snowfall = av->GetAVNodeInfo(BG_AV_NODES_SNOWFALL_GRAVE);
        return snowfall.OwnerId != TEAM_ALLIANCE; // Active if the Snowfall Graveyard is NOT fully controlled by the Alliance
    }

    return false;
}
