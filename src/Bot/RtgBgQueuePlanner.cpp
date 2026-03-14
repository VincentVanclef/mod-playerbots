#include "RtgBgQueuePlanner.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "DatabaseEnv.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <mutex>
#include <fmt/format.h>

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

    struct RTG_BgTemplateTeamBounds
    {
        uint32 minPlayersPerTeam = 0;
        uint32 maxPlayersPerTeam = 0;
    };

    static std::unordered_map<uint32, RTG_BgTemplateTeamBounds>& RTG_GetBgTemplateBoundsCache()
    {
        static std::unordered_map<uint32, RTG_BgTemplateTeamBounds> cache;
        static std::once_flag once;

        std::call_once(once, []()
        {
            if (QueryResult result = WorldDatabase.Query("SELECT `ID`, `MinPlayersPerTeam`, `MaxPlayersPerTeam` FROM `battleground_template`"))
            {
                do
                {
                    Field* fields = result->Fetch();
                    uint32 id = fields[0].Get<uint32>();
                    RTG_BgTemplateTeamBounds bounds;
                    bounds.minPlayersPerTeam = fields[1].Get<uint16>();
                    bounds.maxPlayersPerTeam = fields[2].Get<uint16>();
                    cache[id] = bounds;
                } while (result->NextRow());
            }
        });

        return cache;
    }

    static RTG_BgTemplateTeamBounds RTG_GetBgTemplateTeamBounds(uint32 bgTemplateId, uint32 fallbackMaxPlayersPerTeam)
    {
        RTG_BgTemplateTeamBounds bounds;
        bounds.maxPlayersPerTeam = fallbackMaxPlayersPerTeam;

        auto const& cache = RTG_GetBgTemplateBoundsCache();
        if (auto itr = cache.find(bgTemplateId); itr != cache.end())
        {
            bounds = itr->second;
            if (!bounds.maxPlayersPerTeam)
                bounds.maxPlayersPerTeam = fallbackMaxPlayersPerTeam;
        }

        if (!bounds.minPlayersPerTeam)
        {
            if (bounds.maxPlayersPerTeam <= 5)
                bounds.minPlayersPerTeam = bounds.maxPlayersPerTeam;
            else if (bounds.maxPlayersPerTeam <= 15)
                bounds.minPlayersPerTeam = 5;
            else
                bounds.minPlayersPerTeam = 10;
        }

        return bounds;
    }

    static void RTG_PlannerBreadcrumb(std::string const& message)
    {
        LOG_ERROR("server.loading", "{}", message);
        LOG_WARN("server.loading", "{}", message);
        LOG_INFO("server.loading", "{}", message);
        LOG_ERROR("playerbots", "{}", message);
        LOG_WARN("playerbots", "{}", message);
        LOG_INFO("playerbots", "{}", message);
        std::fprintf(stderr, "%s\n", message.c_str());
        std::fflush(stderr);
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

        RTG_BgTemplateTeamBounds templateBounds = RTG_GetBgTemplateTeamBounds(uint32(bgTypeId), bgTemplate->GetMaxPlayersPerTeam());
        uint32 teamSize = templateBounds.maxPlayersPerTeam;
        uint32 starterFloorPerTeam = std::min<uint32>(teamSize, templateBounds.minPlayersPerTeam);
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
            uint32 allianceCurrent = 0;
            uint32 hordeCurrent = 0;
            uint32 allianceTarget = 0;
            uint32 hordeTarget = 0;
            uint32 realQueuePeak = std::max(queueRealAlliance, queueRealHorde);
            uint32 startupTargetPerTeam = std::min<uint32>(teamSize, std::max(starterFloorPerTeam, realQueuePeak));

            if (activeCurrentAlliance || activeCurrentHorde)
            {
                demandPhase = RTG_BG_PHASE_LIVE_REFILL;
                uint32 liveBalanceTarget = std::max(activeCurrentAlliance, activeCurrentHorde);
                allianceTarget = std::min<uint32>(teamSize, liveBalanceTarget);
                hordeTarget = std::min<uint32>(teamSize, liveBalanceTarget);
                allianceCurrent = activeCurrentAlliance;
                hordeCurrent = activeCurrentHorde;
            }
            else if (hasRealDemand && (queueCurrentAlliance || queueCurrentHorde || bgInfo.activeBgQueue))
            {
                demandPhase = RTG_BG_PHASE_POP_OR_INVITE;
                allianceTarget = startupTargetPerTeam;
                hordeTarget = startupTargetPerTeam;
                allianceCurrent = queueCurrentAlliance;
                hordeCurrent = queueCurrentHorde;
            }
            else if (hasRealDemand)
            {
                demandPhase = RTG_BG_PHASE_STARTER_FILL;
                allianceTarget = startupTargetPerTeam;
                hordeTarget = startupTargetPerTeam;
                allianceCurrent = queueCurrentAlliance;
                hordeCurrent = queueCurrentHorde;
            }

            std::string phaseKey = RTG_MakeBgPhaseKey_Overlay(uint32(queueTypeId), uint32(bracketId));
            uint32 previousPhase = mgr.RTG_GetGlobalEvent(phaseKey);
            mgr.RTG_SetGlobalEvent(phaseKey, demandPhase, ttl);
            if (previousPhase != demandPhase)
            {
                RTG_PlannerBreadcrumb(fmt::format("[RTG][BG][PHASE] queue={} bgTemplate={} bracket={} phase={} targetA={} targetH={} minPerTeam={} maxPerTeam={} queueA={} queueH={} activeA={} activeH={} realQueueA={} realQueueH={}",
                    uint32(queueTypeId), uint32(bgTypeId), uint32(bracketId), RTG_BgPhaseName(demandPhase), allianceTarget, hordeTarget, starterFloorPerTeam, teamSize,
                    queueCurrentAlliance, queueCurrentHorde, activeCurrentAlliance, activeCurrentHorde, queueRealAlliance, queueRealHorde));
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
        RTG_PlannerBreadcrumb(fmt::format("[RTG][BG][DEMAND] totalNeed={} anyRealDemand={} maxBots={} planner=starter_vs_live_handoff source=battleground_template", cappedBgNeedTotal, anyRealBgDemand ? 1 : 0, sPlayerbotAIConfig.rtgEventMaxBots));
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
