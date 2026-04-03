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

        // Split RDF accounting into two truths:
        // 1) acquireNeed*: how many new helpers must still be acquired/logged in
        //    after counting already-assigned helpers.
        // 2) laneNeed*: whether the owner's RDF request lane must remain open so
        //    already-assigned helpers are still allowed to complete their queue join.
        uint32 presentAcquireTank = req.realTank + req.helperQueuedTank + req.helperAssignedTank;
        uint32 presentAcquireHeal = req.realHeal + req.helperQueuedHeal + req.helperAssignedHeal;
        uint32 presentAcquireDps = req.realDps + req.helperQueuedDps + req.helperAssignedDps;

        uint32 acquireNeedTank = presentAcquireTank >= 1 ? 0u : 1u;
        uint32 acquireNeedHeal = presentAcquireHeal >= 1 ? 0u : 1u;
        uint32 acquireNeedDps = presentAcquireDps >= 3 ? 0u : (3u - presentAcquireDps);
        uint32 acquireHelperNeed = acquireNeedTank + acquireNeedHeal + acquireNeedDps;

        uint32 presentLaneTank = req.realTank + req.helperQueuedTank;
        uint32 presentLaneHeal = req.realHeal + req.helperQueuedHeal;
        uint32 presentLaneDps = req.realDps + req.helperQueuedDps;

        uint32 laneNeedTank = presentLaneTank >= 1 ? 0u : 1u;
        uint32 laneNeedHeal = presentLaneHeal >= 1 ? 0u : 1u;
        uint32 laneNeedDps = presentLaneDps >= 3 ? 0u : (3u - presentLaneDps);
        uint32 outstandingAssigned = req.helperAssignedTank + req.helperAssignedHeal + req.helperAssignedDps;

        // Close the owner's request lane only when the run is truly active in-dungeon,
        // or when no queued/live deficit remains and there are no offline assigned
        // helpers still expected to finish the join path.
        bool requestClosed = req.activeDungeon || ((laneNeedTank + laneNeedHeal + laneNeedDps) == 0u && outstandingAssigned == 0u);
        mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_real_demand", requestClosed ? 0u : 1u, requestClosed ? 0u : ownerTtl, ownerAddData);

        if (!requestClosed)
            desiredHelperTotal += acquireHelperNeed;

        if (acquireNeedTank)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_tank",
                acquireNeedTank,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_TANK, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_tank", 0, 0);
        }

        if (acquireNeedHeal)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_heal",
                acquireNeedHeal,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_HEALER, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_heal", 0, 0);
        }

        if (acquireNeedDps)
        {
            mgr.RTG_SetBotEventValue(
                req.owner,
                "rtg_lfg_need_dps",
                acquireNeedDps,
                ownerTtl,
                RTG::MakeLfgAddData(req.team, req.level, lfg::PLAYER_ROLE_DAMAGE, req.owner));
        }
        else
        {
            mgr.RTG_SetBotEventValue(req.owner, "rtg_lfg_need_dps", 0, 0);
        }

        if (sPlayerbotAIConfig.rtgEventDebug || requestClosed)
        {
            LOG_INFO("playerbots",
                "[RTG][LFG][PLAN] owner={} team={} level={} realQueued={} realActive={} helperQueuedT={} helperQueuedH={} helperQueuedD={} helperAssignedT={} helperAssignedH={} helperAssignedD={} needT={} needH={} needD={} startTs={} requestClosed={}",
                req.owner, req.team, req.level,
                req.realQueued, req.realActive,
                req.helperQueuedTank, req.helperQueuedHeal, req.helperQueuedDps,
                req.helperAssignedTank, req.helperAssignedHeal, req.helperAssignedDps,
                acquireNeedTank, acquireNeedHeal, acquireNeedDps, startTs, requestClosed ? 1u : 0u);
        }

        if (!requestClosed)
        {
            if (req.activeDungeon || now >= startTs + sPlayerbotAIConfig.rtgQueueGraceSeconds)
                anyReady = true;
            else if (!oldestPendingStart || startTs < oldestPendingStart)
                oldestPendingStart = startTs;
        }
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