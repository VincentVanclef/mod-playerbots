#include "RtgBgQueuePlanner.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <string>

namespace
{
    enum RTG_BgDemandPhase : uint32
    {
        RTG_BG_PHASE_DORMANT = 0,
        RTG_BG_PHASE_STARTER_FILL = 1,
        RTG_BG_PHASE_POP_OR_INVITE = 2,
        RTG_BG_PHASE_LIVE_REFILL = 3,
    };

    static std::string RTG_MakeBgDemandKey_Overlay(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_need_") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeBgPhaseKey_Overlay(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_phase_") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static char const* RTG_BgPhaseName(uint32 phase)
    {
        switch (phase)
        {
            case RTG_BG_PHASE_STARTER_FILL: return "starter_fill";
            case RTG_BG_PHASE_POP_OR_INVITE: return "pop_or_invite";
            case RTG_BG_PHASE_LIVE_REFILL: return "live_refill";
            default: return "dormant";
        }
    }
}

namespace RTG
{
void RtgBgQueuePlanner::ApplyDemandEvents(RandomPlayerbotMgr& mgr) const
{
    if (!sPlayerbotAIConfig.rtgEventDriven)
        return;

    uint32 ttl = sPlayerbotAIConfig.rtgQueueGraceSeconds + 120;
    uint32 rtgBgNeedTotal = 0;
    bool anyRealBgDemand = false;

    for (auto const& queueTypePair : mgr.BattlegroundData)
    {
        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueTypePair.first);
        if (BattlegroundMgr::BGArenaType(queueTypeId))
            continue;

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            continue;

        uint32 teamSize = bgTemplate->GetMaxPlayersPerTeam();
        for (auto const& bracketIdPair : queueTypePair.second)
        {
            BattlegroundBracketId bracketId = static_cast<BattlegroundBracketId>(bracketIdPair.first);
            BattlegroundInfo const& bgInfo = bracketIdPair.second;
            if (!bgInfo.minLevel)
                continue;

            uint32 queueRealAlliance = bgInfo.bgQueueAlliancePlayerCount;
            uint32 queueRealHorde = bgInfo.bgQueueHordePlayerCount;
            uint32 activeRealAlliance = bgInfo.bgActiveAlliancePlayerCount;
            uint32 activeRealHorde = bgInfo.bgActiveHordePlayerCount;
            uint32 queueCurrentAlliance = bgInfo.bgQueueAlliancePlayerCount + bgInfo.bgQueueAllianceBotCount;
            uint32 queueCurrentHorde = bgInfo.bgQueueHordePlayerCount + bgInfo.bgQueueHordeBotCount;
            uint32 activeCurrentAlliance = bgInfo.bgActiveAlliancePlayerCount + bgInfo.bgActiveAllianceBotCount;
            uint32 activeCurrentHorde = bgInfo.bgActiveHordePlayerCount + bgInfo.bgActiveHordeBotCount;

            bool hasRealDemand = sPlayerbotAIConfig.rtgSmartQueue ?
                ((queueRealAlliance + queueRealHorde + activeRealAlliance + activeRealHorde) > 0) :
                ((bgInfo.bgAlliancePlayerCount + bgInfo.bgHordePlayerCount) > 0);
            bool queueOrMatchActive = sPlayerbotAIConfig.rtgSmartQueue ?
                (bgInfo.activeBgQueue || activeCurrentAlliance || activeCurrentHorde || queueCurrentAlliance || queueCurrentHorde) :
                (bgInfo.activeBgQueue || hasRealDemand);
            mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), hasRealDemand ? 1u : 0u, ttl);
            if (!queueOrMatchActive)
                continue;

            if (hasRealDemand)
                anyRealBgDemand = true;

            uint32 demandPhase = RTG_BG_PHASE_DORMANT;
            uint32 allianceCurrent = sPlayerbotAIConfig.rtgSmartQueue ? std::max(queueCurrentAlliance, activeCurrentAlliance) : (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount);
            uint32 hordeCurrent = sPlayerbotAIConfig.rtgSmartQueue ? std::max(queueCurrentHorde, activeCurrentHorde) : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount);
            uint32 allianceTarget = teamSize;
            uint32 hordeTarget = teamSize;

            if (activeCurrentAlliance || activeCurrentHorde)
            {
                demandPhase = RTG_BG_PHASE_LIVE_REFILL;
                uint32 liveBalanceTarget = std::max(activeCurrentAlliance, activeCurrentHorde);
                allianceTarget = std::min<uint32>(teamSize, liveBalanceTarget);
                hordeTarget = std::min<uint32>(teamSize, liveBalanceTarget);
                allianceCurrent = activeCurrentAlliance;
                hordeCurrent = activeCurrentHorde;
            }
            else if (queueCurrentAlliance || queueCurrentHorde || bgInfo.activeBgQueue)
            {
                demandPhase = RTG_BG_PHASE_POP_OR_INVITE;
                allianceTarget = std::min<uint32>(teamSize, std::max(queueRealAlliance, queueRealHorde));
                hordeTarget = std::min<uint32>(teamSize, std::max(queueRealAlliance, queueRealHorde));
            }
            else if (hasRealDemand)
            {
                demandPhase = RTG_BG_PHASE_STARTER_FILL;
                uint32 replacementCap = 3;
                allianceTarget = std::min<uint32>(teamSize, bgInfo.bgAlliancePlayerCount + replacementCap);
                hordeTarget = std::min<uint32>(teamSize, bgInfo.bgHordePlayerCount + replacementCap);
            }

            std::string phaseKey = RTG_MakeBgPhaseKey_Overlay(uint32(queueTypeId), uint32(bracketId));
            uint32 previousPhase = mgr.RTG_GetGlobalEvent(phaseKey);
            mgr.RTG_SetGlobalEvent(phaseKey, demandPhase, ttl);
            if (previousPhase != demandPhase)
            {
                LOG_WARN("server.loading", "[RTG][BG][PHASE] queue={} bracket={} phase={} queueA={} queueH={} activeA={} activeH={}",
                    uint32(queueTypeId), uint32(bracketId), RTG_BgPhaseName(demandPhase), queueCurrentAlliance, queueCurrentHorde, activeCurrentAlliance, activeCurrentHorde);
                LOG_INFO("playerbots", "[RTG][BG][PHASE] queue={} bracket={} phase={} queueA={} queueH={} activeA={} activeH={}",
                    uint32(queueTypeId), uint32(bracketId), RTG_BgPhaseName(demandPhase), queueCurrentAlliance, queueCurrentHorde, activeCurrentAlliance, activeCurrentHorde);
            }

            if (allianceCurrent < allianceTarget)
                rtgBgNeedTotal += (allianceTarget - allianceCurrent);

            if (hordeCurrent < hordeTarget)
                rtgBgNeedTotal += (hordeTarget - hordeCurrent);
        }
    }

    uint32 cappedBgNeedTotal = std::min<uint32>(rtgBgNeedTotal, sPlayerbotAIConfig.rtgEventMaxBots);
    uint32 previousBgNeedTotal = mgr.RTG_GetGlobalEvent("rtg_bg_need_total");

    mgr.RTG_SetGlobalEvent("rtg_bg_any_real_queued", anyRealBgDemand ? 1u : 0u, ttl);
    mgr.RTG_SetGlobalEvent("rtg_bg_any_real_demand", anyRealBgDemand ? 1u : 0u, ttl);
    mgr.RTG_SetGlobalEvent("rtg_bg_need_total", cappedBgNeedTotal, ttl);

    if (previousBgNeedTotal != cappedBgNeedTotal)
    {
        LOG_WARN("server.loading", "[RTG][BG][DEMAND] totalNeed={} anyRealDemand={} maxBots={}", cappedBgNeedTotal, anyRealBgDemand ? 1 : 0, sPlayerbotAIConfig.rtgEventMaxBots);
        LOG_INFO("playerbots", "[RTG][BG][DEMAND] totalNeed={} anyRealDemand={} maxBots={}", cappedBgNeedTotal, anyRealBgDemand ? 1 : 0, sPlayerbotAIConfig.rtgEventMaxBots);
    }

    if (anyRealBgDemand && rtgBgNeedTotal)
    {
        uint32 existing = mgr.RTG_GetGlobalEvent("rtg_bg_start");
        uint32 now = static_cast<uint32>(time(nullptr));
        uint32 start = existing ? existing : (now - sPlayerbotAIConfig.rtgQueueGraceSeconds);
        mgr.RTG_SetGlobalEvent("rtg_bg_start", start, ttl);
    }
    else
    {
        mgr.RTG_SetGlobalEvent("rtg_bg_start", 0, 0);
		mgr.RTG_SetGlobalEvent("rtg_bg_need_total", 0, 0);
    }
}
}
