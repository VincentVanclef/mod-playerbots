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
    static std::string RTG_MakeBgDemandKey_Overlay(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_need_") + std::to_string(queueType) + ":" + std::to_string(bracketId);
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

            uint32 allianceCurrent = sPlayerbotAIConfig.rtgSmartQueue ? std::max(queueCurrentAlliance, activeCurrentAlliance) : (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount);
            uint32 hordeCurrent = sPlayerbotAIConfig.rtgSmartQueue ? std::max(queueCurrentHorde, activeCurrentHorde) : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount);

            uint32 allianceTarget = teamSize;
            uint32 hordeTarget = teamSize;

            if (!bgInfo.activeBgQueue && hasRealDemand)
            {
                uint32 replacementCap = 3;
                allianceTarget = std::min<uint32>(teamSize, bgInfo.bgAlliancePlayerCount + replacementCap);
                hordeTarget = std::min<uint32>(teamSize, bgInfo.bgHordePlayerCount + replacementCap);
            }

            if (allianceCurrent < allianceTarget)
                rtgBgNeedTotal += (allianceTarget - allianceCurrent);

            if (hordeCurrent < hordeTarget)
                rtgBgNeedTotal += (hordeTarget - hordeCurrent);
        }
    }

    mgr.RTG_SetGlobalEvent("rtg_bg_any_real_queued", anyRealBgDemand ? 1u : 0u, ttl);
	mgr.RTG_SetGlobalEvent("rtg_bg_any_real_demand", anyRealBgDemand ? 1u : 0u, ttl);
	mgr.RTG_SetGlobalEvent("rtg_bg_need_total", std::min<uint32>(rtgBgNeedTotal, sPlayerbotAIConfig.rtgEventMaxBots), ttl);

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
