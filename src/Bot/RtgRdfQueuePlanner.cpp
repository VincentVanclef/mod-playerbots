#include "RtgRdfQueuePlanner.h"

#include "PlayerbotAIConfig.h"
#include "LFG.h"
#include "RandomPlayerbotMgr.h"
#include "RtgQueueMetadata.h"

#include <algorithm>
#include <ctime>

namespace RTG
{
void RtgRdfQueuePlanner::ApplyDemandEvents(RandomPlayerbotMgr& mgr,
    std::map<uint32, RtgLfgQueueOwnerSnapshot> const& requests,
    bool anyRealLfgDemand) const
{
    if (!sPlayerbotAIConfig.rtgEventDriven)
        return;

    uint32 now = static_cast<uint32>(time(nullptr));
    uint32 ownerTtl = sPlayerbotAIConfig.rtgQueueGraceSeconds + 20;
    uint32 globalTtl = sPlayerbotAIConfig.rtgQueueGraceSeconds + 120;
    uint32 desiredHelperTotal = 0;
    bool anyReady = false;
    uint32 oldestPendingStart = 0;

    for (auto const& kv : requests)
    {
        RtgLfgQueueOwnerSnapshot const& req = kv.second;

        uint32 existingStart = mgr.RTG_GetBotEventValue(req.owner, "rtg_lfg_start");
        uint32 startTs = req.activeDungeon
            ? (now - sPlayerbotAIConfig.rtgQueueGraceSeconds)
            : (existingStart ? existingStart : now);

        std::string ownerAddData = RTG::MakeLfgAddData(req.team, req.level, 0, req.owner);

        mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_start", startTs, ownerTtl, ownerAddData);
        mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_real_demand", 1u, ownerTtl, ownerAddData);

        uint32 needTank = req.realTank >= 1 ? 0u : 1u;
        uint32 needHeal = req.realHeal >= 1 ? 0u : 1u;
        uint32 needDps = req.realDps >= 3 ? 0u : (3u - req.realDps);
        uint32 helperNeed = needTank + needHeal + needDps;

        desiredHelperTotal += helperNeed;

        if (needTank)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_tank",
                needTank,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_TANK, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_tank", 0, 0);
        }

        if (needHeal)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_heal",
                needHeal,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_HEALER, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_heal", 0, 0);
        }

        if (needDps)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_dps",
                needDps,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_DAMAGE, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_dps", 0, 0);
        }

        if (sPlayerbotAIConfig.rtgEventDebug || helperNeed)
        {
            LOG_INFO("playerbots",
                "[RTG][LFG][PLAN] owner={} team={} level={} realQueued={} realActive={} needT={} needH={} needD={} startTs={}",
                req.owner, req.team, req.level,
                req.realQueued, req.realActive,
                needTank, needHeal, needDps, startTs);
        }

        if (req.activeDungeon || now >= startTs + sPlayerbotAIConfig.rtgQueueGraceSeconds)
            anyReady = true;
        else if (!oldestPendingStart || startTs < oldestPendingStart)
            oldestPendingStart = startTs;
    }

    if (anyRealLfgDemand && desiredHelperTotal)
    {
        uint32 globalStart = anyReady
            ? (now - sPlayerbotAIConfig.rtgQueueGraceSeconds)
            : (oldestPendingStart ? oldestPendingStart : now);

        uint32 cappedNeed = std::min<uint32>(desiredHelperTotal, sPlayerbotAIConfig.rtgLfgMaxBots);

        LOG_INFO("playerbots",
            "[RTG][LFG][TOTAL] demandOwners={} desiredHelpers={} cappedHelpers={} anyReady={} globalStart={}",
            static_cast<uint32>(requests.size()), desiredHelperTotal, cappedNeed, anyReady ? 1u : 0u, globalStart);

        mgr.RTG_SetBotEventValue(0, "rtg_lfg_start", globalStart, globalTtl);
        mgr.RTG_SetBotEventValue(0, "rtg_lfg_need_total", cappedNeed, globalTtl);
    }
    else
    {
        mgr.RTG_SetBotEventValue(0, "rtg_lfg_start", 0, 0);
        mgr.RTG_SetBotEventValue(0, "rtg_lfg_need_total", 0, 0);
    }
}
}