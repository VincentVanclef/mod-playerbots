#include "RtgBgQueuePlanner.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace
{
    static uint32 RTG_ClampBgPhaseTeamNeed(uint32 phase, uint32 need)
    {
        if (!need)
            return 0u;

        // Seed/start phases can fill aggressively so a battleground actually opens.
        // Once the match is live, refill in smaller chunks to preserve room for real
        // players and avoid overreacting to temporary join/teleport skew.
        if (phase >= 4u)
            return std::min<uint32>(need, 4u);
        if (phase == 3u)
            return std::min<uint32>(need, 3u);

        return need;
    }

    struct RTG_LiveBgDemand
    {
        uint32 queueAlliance = 0;
        uint32 queueHorde = 0;
    };

    struct RTG_BgTemplateBounds
    {
        uint32 minPerTeam = 0;
        uint32 maxPerTeam = 0;
    };

    static std::string RTG_MakeBgDemandKey_Overlay(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_need_") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeBgRealDemandKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_real_demand:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeBgTeamNeedKey(uint32 queueType, uint32 bracketId, uint32 teamId)
    {
        return std::string("rtg_bg_team_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId) + ":" + std::to_string(teamId);
    }

    static std::string RTG_MakeBgPhaseKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_phase:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeArenaDemandKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_arena_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeArenaTeamNeedKey(uint32 queueType, uint32 bracketId, uint32 teamId)
    {
        return std::string("rtg_arena_team_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId) + ":" + std::to_string(teamId);
    }

    static std::string RTG_MakeArenaPhaseKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_arena_phase:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeArenaRecentActivityKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_arena_recent_activity:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static constexpr uint32 RTG_ARENA_DORMANT_CLEAR_GRACE_SECONDS = 10u;

    static std::string RTG_MakeBgActiveStartKey(uint32 queueType, uint32 bracketId)
    {
        return std::string("rtg_bg_active_start:") + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static char const* RTG_GetBgPhaseName(uint32 phase)
    {
        switch (phase)
        {
            case 1: return "starter_fill";
            case 2: return "pop_or_invite";
            case 3: return "live_refill";
            case 4: return "finish_fill";
            default: return "dormant";
        }
    }

    static char const* RTG_GetBgPlannerObjective(uint32 phase)
    {
        switch (phase)
        {
            case 1: return "starter_fill";
            case 2: return "pop_or_invite";
            case 3: return "live_refill";
            case 4: return "finish_fill";
            default: return "dormant";
        }
    }

    template <typename... Args>
    static void RTG_WorldLog(std::string const& fmt, Args&&... args)
    {
        LOG_INFO("server.loading", fmt, std::forward<Args>(args)...);
    }

    static uint64 RTG_MakeSeenKey(uint32 queueType, uint32 bracketId)
    {
        return (uint64(queueType) << 32) | uint64(bracketId);
    }

    static RTG_BgTemplateBounds RTG_GetFallbackBounds(Battleground* bgTemplate)
    {
        RTG_BgTemplateBounds bounds;
        if (!bgTemplate)
            return bounds;

        bounds.maxPerTeam = bgTemplate->GetMaxPlayersPerTeam();
        bounds.minPerTeam = bounds.maxPerTeam;
        return bounds;
    }

    static RTG_BgTemplateBounds RTG_GetBgTemplateBounds(BattlegroundTypeId bgTypeId, Battleground* bgTemplate)
    {
        static std::once_flag once;
        static std::unordered_map<uint32, RTG_BgTemplateBounds> cache;

        std::call_once(once, []()
        {
            QueryResult result = WorldDatabase.Query("SELECT `ID`, `MinPlayersPerTeam`, `MaxPlayersPerTeam` FROM `battleground_template`");
            if (!result)
                return;

            do
            {
                Field* fields = result->Fetch();
                RTG_BgTemplateBounds bounds;
                uint32 id = fields[0].Get<uint32>();
                bounds.minPerTeam = fields[1].Get<uint16>();
                bounds.maxPerTeam = fields[2].Get<uint16>();
                cache[id] = bounds;
            } while (result->NextRow());
        });

        auto it = cache.find(uint32(bgTypeId));
        if (it != cache.end())
        {
            RTG_BgTemplateBounds bounds = it->second;
            if (!bounds.maxPerTeam && bgTemplate)
                bounds.maxPerTeam = bgTemplate->GetMaxPlayersPerTeam();
            if (!bounds.minPerTeam)
                bounds.minPerTeam = bounds.maxPerTeam;
            if (bounds.minPerTeam > bounds.maxPerTeam)
                bounds.minPerTeam = bounds.maxPerTeam;
            return bounds;
        }

        return RTG_GetFallbackBounds(bgTemplate);
    }

    static uint32 RTG_NormalizeArenaTeamSize(BattlegroundQueueTypeId queueTypeId)
    {
        uint32 raw = uint32(BattlegroundMgr::BGArenaType(queueTypeId));
        if (raw == 4u)
            return 3u;
        if (raw == 1u)
            return 1u;
        if (queueTypeId == BATTLEGROUND_QUEUE_2v2)
            return 2u;
        if (queueTypeId == BATTLEGROUND_QUEUE_3v3)
            return 3u;
        if (queueTypeId == BATTLEGROUND_QUEUE_5v5)
            return 5u;
        if (uint32(queueTypeId) == 9u)
            return 3u;
        return raw;
    }

    static uint32 RTG_GetArenaConcurrentMatchCap()
    {
        return 3u;
    }

    static bool RTG_PlayerHasCompetingPvpQueue(Player* player, BattlegroundQueueTypeId arenaQueueTypeId)
    {
        if (!player)
            return false;

        for (uint8 queueSlot = 0; queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueSlot)
        {
            BattlegroundQueueTypeId queuedType = player->GetBattlegroundQueueTypeId(queueSlot);
            if (queuedType <= BATTLEGROUND_QUEUE_NONE || queuedType >= MAX_BATTLEGROUND_QUEUE_TYPES)
                continue;

            if (queuedType != arenaQueueTypeId)
                return true;
        }

        return false;
    }

    static bool RTG_PlayerHasLfgCommitment(Player* player)
    {
        if (!player)
            return false;

        Group* group = player->GetGroup();
        if (group)
        {
            lfg::LfgState groupState = sLFGMgr->GetState(group->GetGUID());
            if (group->isLFGGroup() || groupState != lfg::LFG_STATE_NONE)
                return true;
        }

        return sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE;
    }

    static bool RTG_ShouldDeferArenaOwner(Player* owner, BattlegroundQueueTypeId arenaQueueTypeId, char const*& reason)
    {
        reason = "none";
        if (!owner || !owner->IsInWorld())
        {
            reason = "owner_offline";
            return true;
        }

        if (owner->InArena())
        {
            reason = "owner_in_arena";
            return true;
        }

        if (owner->InBattleground())
        {
            reason = "owner_in_battleground";
            return true;
        }

        if (owner->IsInvitedForBattlegroundInstance())
        {
            reason = "owner_bg_invited";
            return true;
        }

        Map* map = owner->GetMap();
        if (map && (map->IsDungeon() || map->IsRaid()))
        {
            reason = "owner_in_dungeon";
            return true;
        }

        if (RTG_PlayerHasLfgCommitment(owner))
        {
            reason = "owner_lfg_active";
            return true;
        }

        if (RTG_PlayerHasCompetingPvpQueue(owner, arenaQueueTypeId))
        {
            reason = "owner_competing_pvp_queue";
            return true;
        }

        return false;
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
    std::set<uint64> seenKeys;
    static std::set<uint64> sPrevSeenKeys;

    std::map<std::pair<uint32, uint32>, RTG_LiveBgDemand> liveBgRealDemand;
    std::map<std::pair<uint32, uint32>, RTG_LiveBgDemand> liveArenaRealDemand;
    for (Player* player : mgr.GetPlayers())
    {
        if (!player || !player->IsInWorld() || mgr.IsRandomBot(player))
            continue;

        // Only count real players who are actually still waiting in a queue.
        // Once they are invited or already inside a battleground/arena, that state
        // must not be recycled into fresh helper demand.
        if (player->InBattleground() || player->InArena() || player->IsInvitedForBattlegroundInstance())
            continue;

        for (uint8 queueSlot = 0; queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueSlot)
        {
            BattlegroundQueueTypeId liveQueueType = player->GetBattlegroundQueueTypeId(queueSlot);
            if (liveQueueType <= BATTLEGROUND_QUEUE_NONE || liveQueueType >= MAX_BATTLEGROUND_QUEUE_TYPES)
                continue;

            BattlegroundTypeId liveBgType = BattlegroundMgr::BGTemplateId(liveQueueType);
            if (liveBgType == BATTLEGROUND_TYPE_NONE)
                continue;

            Battleground* liveTemplate = sBattlegroundMgr->GetBattlegroundTemplate(liveBgType);
            if (!liveTemplate)
                continue;

            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(liveTemplate->GetMapId(), player->GetLevel());
            if (!pvpDiff)
                continue;

            bool isArenaQueue = RTG_NormalizeArenaTeamSize(liveQueueType) != 0u;
            if (isArenaQueue)
            {
                char const* deferReason = nullptr;
                if (RTG_ShouldDeferArenaOwner(player, liveQueueType, deferReason))
                {
                    RTG_WorldLog("[RTG][ARENA][OWNER_DEFER] queue={} bracket={} owner={} player={} reason={} source=planner_live_scan",
                        uint32(liveQueueType), uint32(pvpDiff->GetBracketId()), player->GetGUID().GetCounter(),
                        player->GetGUID().GetCounter(), deferReason ? deferReason : "unknown");
                    continue;
                }
            }

            auto& live = isArenaQueue
                ? liveArenaRealDemand[std::make_pair(uint32(liveQueueType), uint32(pvpDiff->GetBracketId()))]
                : liveBgRealDemand[std::make_pair(uint32(liveQueueType), uint32(pvpDiff->GetBracketId()))];
            if (player->GetTeamId() == TEAM_ALLIANCE)
                ++live.queueAlliance;
            else
                ++live.queueHorde;
        }
    }

    for (auto const& queueTypePair : mgr.BattlegroundData)
    {
        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueTypePair.first);

        if (uint32 arenaTeamSize = RTG_NormalizeArenaTeamSize(queueTypeId))
        {
            for (auto const& bracketIdPair : queueTypePair.second)
            {
                BattlegroundBracketId bracketId = static_cast<BattlegroundBracketId>(bracketIdPair.first);
                seenKeys.insert(RTG_MakeSeenKey(uint32(queueTypeId), uint32(bracketId)));

                BattlegroundInfo const& bgInfo = bracketIdPair.second;
                if (!bgInfo.minLevel)
                    continue;

                bool isRated = bgInfo.ratedArenaPlayerCount || bgInfo.ratedArenaBotCount ||
                               bgInfo.activeRatedArenaQueue || bgInfo.ratedArenaInstanceCount;
                uint32 realQueued = 0;
                uint32 realQueuedAlliance = 0;
                uint32 realQueuedHorde = 0;
                auto liveItr = liveArenaRealDemand.find(std::make_pair(uint32(queueTypeId), uint32(bracketId)));
                if (liveItr != liveArenaRealDemand.end())
                {
                    realQueuedAlliance = liveItr->second.queueAlliance;
                    realQueuedHorde = liveItr->second.queueHorde;
                    realQueued = realQueuedAlliance + realQueuedHorde;
                }
                uint32 activeInstances = isRated ? bgInfo.ratedArenaInstanceCount : bgInfo.skirmishArenaInstanceCount;
                bool activeMatch = activeInstances > 0;
                bool ignoreActiveMatchBotsForFreshDemand = activeMatch && realQueued > 0u;
                uint32 botQueuedAlliance = ignoreActiveMatchBotsForFreshDemand ? 0u :
                    (isRated ? bgInfo.ratedArenaAllianceBotCount : bgInfo.skirmishArenaAllianceBotCount);
                uint32 botQueuedHorde = ignoreActiveMatchBotsForFreshDemand ? 0u :
                    (isRated ? bgInfo.ratedArenaHordeBotCount : bgInfo.skirmishArenaHordeBotCount);
                uint32 currentQueuedAlliance = realQueuedAlliance + botQueuedAlliance;
                uint32 currentQueuedHorde = realQueuedHorde + botQueuedHorde;
                uint32 currentQueued = currentQueuedAlliance + currentQueuedHorde;
                uint32 targetConcurrentMatches = std::min<uint32>(RTG_GetArenaConcurrentMatchCap(), activeInstances + realQueued);
                if (realQueued > 0)
                    targetConcurrentMatches = std::max<uint32>(1u, targetConcurrentMatches);
                bool hardDormant = (realQueued == 0u && !activeMatch);
                bool hasRealDemand = realQueued > 0 && activeInstances < targetConcurrentMatches;
                bool hasQueueSeed = currentQueued > 0;
                uint32 nowTs = static_cast<uint32>(std::time(nullptr));
                std::string recentActivityKey = RTG_MakeArenaRecentActivityKey(uint32(queueTypeId), uint32(bracketId));
                uint32 recentActivity = mgr.RTG_GetGlobalEvent(recentActivityKey);
                if (realQueued > 0u || activeMatch)
                {
                    recentActivity = nowTs;
                    mgr.RTG_SetGlobalEvent(recentActivityKey, recentActivity, 60u);
                }
                bool recentArenaActivity = recentActivity &&
                    nowTs >= recentActivity &&
                    (nowTs - recentActivity) < RTG_ARENA_DORMANT_CLEAR_GRACE_SECONDS;
                hardDormant = hardDormant && !recentArenaActivity;
                bool orphanQueueResidue = !recentArenaActivity &&
                    (hardDormant ? (currentQueued > 0) : (!hasRealDemand && !activeMatch && currentQueued > 0));

                // Hard planner gate: once arena is truly dormant (no real queue, no active instance,
                // no queued helpers), clear state and stop processing entirely.
                if (hardDormant && !hasQueueSeed)
                {
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgPhaseKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0, "dormant");
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaPhaseKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0, "dormant");
                    continue;
                }

                mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeArenaDemandKey(uint32(queueTypeId), uint32(bracketId)), hasRealDemand ? 1u : 0u, hasRealDemand ? ttl : 0);

                uint32 desiredTotal = hasRealDemand ? (arenaTeamSize * 2u) : 0u;
                uint32 totalNeed = (hasRealDemand && desiredTotal > currentQueued) ? (desiredTotal - currentQueued) : 0u;
                uint32 allianceNeed = hasRealDemand ? (arenaTeamSize > currentQueuedAlliance ? (arenaTeamSize - currentQueuedAlliance) : 0u) : 0u;
                uint32 hordeNeed = hasRealDemand ? (arenaTeamSize > currentQueuedHorde ? (arenaTeamSize - currentQueuedHorde) : 0u) : 0u;
                if (hasRealDemand && allianceNeed + hordeNeed != totalNeed)
                {
                    allianceNeed = std::min(allianceNeed, totalNeed);
                    hordeNeed = totalNeed > allianceNeed ? (totalNeed - allianceNeed) : 0u;
                }
                // Arena is a one-time fill lane, not a BG-style live_refill lane.
                uint32 phase = hasRealDemand ? 2u : 0u;

                if (orphanQueueResidue)
                {
                    RTG_WorldLog("[RTG][ARENA][CLEAR] queue={} bracket={} reason={} queued={} activeInstances={} realQueued={}",
                        uint32(queueTypeId), uint32(bracketId), hardDormant ? "hard_dormant" : "orphan_queue_residue", currentQueued, activeInstances, realQueued);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgPhaseKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0, "dormant");
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeArenaPhaseKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0, "dormant");
                    continue;
                }

                mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), 0u, 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), 0u, 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeBgPhaseKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0, "dormant");
                mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), allianceNeed, allianceNeed ? ttl : 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeArenaTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), hordeNeed, hordeNeed ? ttl : 0);
                mgr.RTG_SetGlobalEvent(RTG_MakeArenaPhaseKey(uint32(queueTypeId), uint32(bracketId)), phase, phase ? ttl : 0, RTG_GetBgPhaseName(phase));

                RTG_WorldLog("[RTG][ARENA][PHASE] queue={} bracket={} phase={} teamSize={} queued={} realQueued={} activeInstances={} rated={}",
                    uint32(queueTypeId), uint32(bracketId), RTG_GetBgPhaseName(phase), arenaTeamSize, currentQueued, realQueued, activeInstances, isRated ? 1u : 0u);
                RTG_WorldLog("[RTG][ARENA][DEMAND] queue={} bracket={} needA={} needH={} totalNeed={} anyRealDemand={} rated={} targetMatches={}",
                    uint32(queueTypeId), uint32(bracketId), allianceNeed, hordeNeed, allianceNeed + hordeNeed, hasRealDemand ? 1u : 0u, isRated ? 1u : 0u, targetConcurrentMatches);

                if (activeMatch && !hasRealDemand)
                    continue;

                if (!hasQueueSeed)
                    continue;

                if (hasRealDemand)
                    anyRealBgDemand = true;

                rtgBgNeedTotal += allianceNeed + hordeNeed;
            }

            continue;
        }

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            continue;

        RTG_BgTemplateBounds bounds = RTG_GetBgTemplateBounds(bgTypeId, bgTemplate);
        uint32 minPerTeam = bounds.minPerTeam;
        uint32 maxPerTeam = bounds.maxPerTeam;

        for (auto const& bracketIdPair : queueTypePair.second)
        {
            BattlegroundBracketId bracketId = static_cast<BattlegroundBracketId>(bracketIdPair.first);
            seenKeys.insert(RTG_MakeSeenKey(uint32(queueTypeId), uint32(bracketId)));
            BattlegroundInfo const& bgInfo = bracketIdPair.second;
            if (!bgInfo.minLevel)
                continue;

            uint32 queueRealAlliance = bgInfo.bgQueueAlliancePlayerCount;
            uint32 queueRealHorde = bgInfo.bgQueueHordePlayerCount;
            auto liveItr = liveBgRealDemand.find(std::make_pair(uint32(queueTypeId), uint32(bracketId)));
            if (liveItr != liveBgRealDemand.end())
            {
                queueRealAlliance = liveItr->second.queueAlliance;
                queueRealHorde = liveItr->second.queueHorde;
            }
            uint32 activeRealAlliance = bgInfo.bgActiveAlliancePlayerCount;
            uint32 activeRealHorde = bgInfo.bgActiveHordePlayerCount;
            uint32 queueCurrentAlliance = bgInfo.bgQueueAlliancePlayerCount + bgInfo.bgQueueAllianceBotCount;
            uint32 queueCurrentHorde = bgInfo.bgQueueHordePlayerCount + bgInfo.bgQueueHordeBotCount;
            uint32 activeCurrentAlliance = bgInfo.bgActiveAlliancePlayerCount + bgInfo.bgActiveAllianceBotCount;
            uint32 activeCurrentHorde = bgInfo.bgActiveHordePlayerCount + bgInfo.bgActiveHordeBotCount;

            bool hasRealDemand = sPlayerbotAIConfig.rtgSmartQueue ?
                ((queueRealAlliance + queueRealHorde + activeRealAlliance + activeRealHorde) > 0) :
                ((bgInfo.bgAlliancePlayerCount + bgInfo.bgHordePlayerCount) > 0);
            bool hasRealQueuedSeed = (queueRealAlliance || queueRealHorde);
            bool hasActiveMatch = (activeCurrentAlliance || activeCurrentHorde);
            bool hasRealActiveMatch = (activeRealAlliance || activeRealHorde);
            bool orphanQueueResidue = !hasRealDemand && (queueCurrentAlliance || queueCurrentHorde || activeCurrentAlliance || activeCurrentHorde || bgInfo.activeBgQueue);
            bool queueOrMatchActive = sPlayerbotAIConfig.rtgSmartQueue ?
                (hasRealActiveMatch || hasRealQueuedSeed) :
                (bgInfo.activeBgQueue || hasRealDemand);

            mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), hasRealDemand ? 1u : 0u, ttl);
            mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(uint32(queueTypeId), uint32(bracketId)), hasRealDemand ? 1u : 0u, ttl);

            uint32 allianceNeed = 0;
            uint32 hordeNeed = 0;
            uint32 phase = 0;
            uint32 allianceTarget = 0;
            uint32 hordeTarget = 0;
            uint32 nowSecs = static_cast<uint32>(time(nullptr));
            uint32 activeStart = mgr.RTG_GetGlobalEvent(RTG_MakeBgActiveStartKey(uint32(queueTypeId), uint32(bracketId)));

            if (hasActiveMatch && hasRealDemand)
            {
                if (!activeStart)
                    activeStart = nowSecs;
                mgr.RTG_SetGlobalEvent(RTG_MakeBgActiveStartKey(uint32(queueTypeId), uint32(bracketId)), activeStart, 7200);

                uint32 elapsed = activeStart && nowSecs >= activeStart ? (nowSecs - activeStart) : 0u;
                uint32 rampSteps = elapsed / 20u;
                uint32 rampTarget = minPerTeam + (rampSteps * 4u);
                uint32 liveTarget = std::max(activeCurrentAlliance, activeCurrentHorde);
                liveTarget = std::max(liveTarget, minPerTeam);

                uint32 symmetricActive = std::min(activeCurrentAlliance, activeCurrentHorde);
                uint32 growthGuardTarget = symmetricActive >= minPerTeam ? (symmetricActive + 4u) : minPerTeam;

                if (maxPerTeam)
                {
                    rampTarget = std::min(rampTarget, maxPerTeam);
                    liveTarget = std::min(liveTarget, maxPerTeam);
                    growthGuardTarget = std::min(growthGuardTarget, maxPerTeam);
                }

                uint32 matureTarget = std::max(liveTarget, minPerTeam);
                matureTarget = std::max(matureTarget, std::min(rampTarget, growthGuardTarget));
                if (maxPerTeam)
                    matureTarget = std::min(matureTarget, maxPerTeam);

                phase = matureTarget > minPerTeam ? 4u : 3u;
                allianceTarget = matureTarget;
                hordeTarget = matureTarget;
                // Live refill can wait on queued helpers, but finish-fill must use active
                // bodies only. Otherwise stale queue slots suppress the last missing side.
                uint32 committedAlliance = phase >= 4u
                    ? std::min(allianceTarget, activeCurrentAlliance)
                    : std::min(allianceTarget, std::max(activeCurrentAlliance, queueCurrentAlliance));
                uint32 committedHorde = phase >= 4u
                    ? std::min(hordeTarget, activeCurrentHorde)
                    : std::min(hordeTarget, std::max(activeCurrentHorde, queueCurrentHorde));
                allianceNeed = allianceTarget > committedAlliance ? (allianceTarget - committedAlliance) : 0u;
                hordeNeed = hordeTarget > committedHorde ? (hordeTarget - committedHorde) : 0u;
                allianceNeed = RTG_ClampBgPhaseTeamNeed(phase, allianceNeed);
                hordeNeed = RTG_ClampBgPhaseTeamNeed(phase, hordeNeed);
            }
            else if (hasRealQueuedSeed)
            {
                mgr.RTG_SetGlobalEvent(RTG_MakeBgActiveStartKey(uint32(queueTypeId), uint32(bracketId)), 0, 0);
                phase = (queueCurrentAlliance || queueCurrentHorde || bgInfo.activeBgQueue) ? 2u : 1u; // pop_or_invite : starter_fill
                uint32 realQueuePeak = std::max(queueRealAlliance, queueRealHorde);
                uint32 startupTarget = std::max(minPerTeam, realQueuePeak);
                if (maxPerTeam)
                    startupTarget = std::min(startupTarget, maxPerTeam);
                allianceTarget = startupTarget;
                hordeTarget = startupTarget;
                allianceNeed = allianceTarget > queueCurrentAlliance ? (allianceTarget - queueCurrentAlliance) : 0u;
                hordeNeed = hordeTarget > queueCurrentHorde ? (hordeTarget - queueCurrentHorde) : 0u;
                allianceNeed = RTG_ClampBgPhaseTeamNeed(phase, allianceNeed);
                hordeNeed = RTG_ClampBgPhaseTeamNeed(phase, hordeNeed);
            }
            else
            {
                mgr.RTG_SetGlobalEvent(RTG_MakeBgActiveStartKey(uint32(queueTypeId), uint32(bracketId)), 0, 0);
                if (orphanQueueResidue)
                {
                    static std::unordered_map<uint64, uint32> sNextOrphanResidueLogAt;
                    uint64 residueKey = (uint64(uint32(queueTypeId)) << 32) | uint64(uint32(bracketId));
                    uint32 nowTs = static_cast<uint32>(time(nullptr));
                    uint32& nextLogAt = sNextOrphanResidueLogAt[residueKey];
                    if (!nextLogAt || nowTs >= nextLogAt)
                    {
                        RTG_WorldLog("[RTG][BG][CLEAR] queue={} bracket={} reason=orphan_queue_residue queueA={} queueH={} activeA={} activeH={}",
                            uint32(queueTypeId), uint32(bracketId), queueCurrentAlliance, queueCurrentHorde, activeCurrentAlliance, activeCurrentHorde);
                        nextLogAt = nowTs + 60;
                    }

                    queueOrMatchActive = false;
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                    mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(uint32(queueTypeId), uint32(bracketId)), 0u, 0);
                }
            }

            mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_ALLIANCE)), allianceNeed, allianceNeed ? ttl : 0);
            mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(uint32(queueTypeId), uint32(bracketId), uint32(TEAM_HORDE)), hordeNeed, hordeNeed ? ttl : 0);
            mgr.RTG_SetGlobalEvent(RTG_MakeBgPhaseKey(uint32(queueTypeId), uint32(bracketId)), phase, phase ? ttl : 0, RTG_GetBgPhaseName(phase));

            if (!queueOrMatchActive)
                continue;

            if (hasRealDemand)
                anyRealBgDemand = true;

            rtgBgNeedTotal += allianceNeed + hordeNeed;

            RTG_WorldLog("[RTG][BG][PHASE] queue={} bracket={} phase={} targetA={} targetH={} queueA={} queueH={} activeA={} activeH={} realQueueA={} realQueueH={} minPerTeam={} maxPerTeam={}",
                     uint32(queueTypeId), uint32(bracketId), RTG_GetBgPhaseName(phase), allianceTarget, hordeTarget,
                     queueCurrentAlliance, queueCurrentHorde, activeCurrentAlliance, activeCurrentHorde,
                     queueRealAlliance, queueRealHorde, minPerTeam, maxPerTeam);
            RTG_WorldLog("[RTG][BG][DEMAND] queue={} bracket={} needA={} needH={} totalNeed={} anyRealDemand={} maxBots={} planner={}",
                     uint32(queueTypeId), uint32(bracketId), allianceNeed, hordeNeed, allianceNeed + hordeNeed, hasRealDemand ? 1u : 0u,
                     sPlayerbotAIConfig.rtgBgMaxBots, RTG_GetBgPlannerObjective(phase));
        }
    }

    for (uint64 staleKey : sPrevSeenKeys)
    {
        if (seenKeys.find(staleKey) != seenKeys.end())
            continue;

        uint32 queueType = uint32(staleKey >> 32);
        uint32 bracketId = uint32(staleKey & 0xFFFFFFFFu);
        mgr.RTG_SetGlobalEvent(RTG_MakeBgDemandKey_Overlay(queueType, bracketId), 0, 0);
        mgr.RTG_SetGlobalEvent(RTG_MakeBgRealDemandKey(queueType, bracketId), 0, 0);
        mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(queueType, bracketId, uint32(TEAM_ALLIANCE)), 0, 0);
        mgr.RTG_SetGlobalEvent(RTG_MakeBgTeamNeedKey(queueType, bracketId, uint32(TEAM_HORDE)), 0, 0);
        mgr.RTG_SetGlobalEvent(RTG_MakeBgPhaseKey(queueType, bracketId), 0, 0, "dormant");
        if (sPlayerbotAIConfig.rtgEventDebug)
            RTG_WorldLog("[RTG][BG][CLEAR] queue={} bracket={} reason=stale_key", queueType, bracketId);
    }
    sPrevSeenKeys = seenKeys;

    mgr.RTG_SetGlobalEvent("rtg_bg_any_real_queued", anyRealBgDemand ? 1u : 0u, ttl);
    mgr.RTG_SetGlobalEvent("rtg_bg_any_real_demand", anyRealBgDemand ? 1u : 0u, ttl);
    mgr.RTG_SetGlobalEvent("rtg_bg_need_total", std::min<uint32>(rtgBgNeedTotal, sPlayerbotAIConfig.rtgBgMaxBots), ttl);

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
