/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RandomPlayerbotMgr.h"

#include <WorldSessionMgr.h>
#include "unordered_set"
#include <algorithm>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <random>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <map>
#include <tuple>
#include <fmt/format.h>

#include "AccountMgr.h"
#include "AiFactory.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CellImpl.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DatabaseEnv.h"
#include "Define.h"
#include "Event.h"
#include "FleeManager.h"
#include "FlightMasterCache.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GuildMgr.h"
#include "GuildTaskMgr.h"
#include "LFGMgr.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotCommandServer.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Position.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "RtgQueueMetadata.h"
#include "RtgBgQueuePlanner.h"
#include "RtgQueueLedger.h"
#include "RtgQueueLifecycle.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "UpdateTime.h"
#include "World.h"

namespace
{
    static bool RTG_QueueDebugEnabled()
    {
        return sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.rtgEventDebug;
    }

    static bool RTG_QueueOwnershipDebugEnabled()
    {
        return sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.rtgQueueOwnershipEnable && sPlayerbotAIConfig.rtgQueueOwnershipDebug;
    }

    static void RTG_RuntimeBreadcrumb(std::string const& message)
    {
        LOG_WARN("playerbots", "{}", message);
        LOG_INFO("playerbots", "{}", message);
        LOG_INFO("server.loading", "{}", message);
    }

    static uint32 RTG_GetQueueGraceTtlSeconds()
    {
        return std::max<uint32>(20u, sPlayerbotAIConfig.rtgQueueGraceSeconds);
    }

    static uint32 RTG_GetStandaloneHelperCeiling()
    {
        if (sPlayerbotAIConfig.rtgEventDriven && !sPlayerbotAIConfig.randomBotAutologin)
            return std::max<uint32>(1u, sPlayerbotAIConfig.rtgEventMaxBots);

        return std::max<uint32>(1u, sPlayerbotAIConfig.maxRandomBots);
    }

    static uint32 RTG_GetQueueRetryWindowSeconds()
    {
        return std::max<uint32>(5u, std::min<uint32>(10u, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds));
    }

    static uint32 RTG_GetDispatchStallThresholdSeconds()
    {
        return std::max<uint32>(20u, RTG_GetQueueGraceTtlSeconds() + 10u);
    }

    static uint32 RTG_GetPendingHelperLoginGlobalCap()
    {
        uint32 ceiling = RTG_GetStandaloneHelperCeiling();
        if (sPlayerbotAIConfig.rtgEventDriven)
            return std::max<uint32>(24u, std::min<uint32>(64u, ceiling));
        return std::max<uint32>(8u, std::min<uint32>(16u, std::max<uint32>(8u, ceiling / 4u)));
    }

    static uint32 RTG_GetPendingHelperLoginLaneCap(uint32 phase)
    {
        if (sPlayerbotAIConfig.rtgEventDriven)
        {
            if (phase >= 4u)
                return 12u;
            if (phase == 3u)
                return 10u;
            return std::max<uint32>(10u, std::min<uint32>(16u, RTG_GetPendingHelperLoginGlobalCap()));
        }

        if (phase >= 4u)
            return 4u;
        if (phase == 3u)
            return 6u;
        return std::max<uint32>(8u, std::min<uint32>(12u, RTG_GetPendingHelperLoginGlobalCap()));
    }

    static bool RTG_IsTrackedPendingHelperState(RTG::RtgHelperLedgerEntry const& entry)
    {
        if (!entry.isEventDrivenHelper || entry.pendingRetire)
            return false;

        if (entry.state == RTG::RtgHelperState::Retired || entry.state == RTG::RtgHelperState::InBattleground)
            return false;

        return entry.pendingQueueJoin || entry.state == RTG::RtgHelperState::Reserved ||
               entry.state == RTG::RtgHelperState::LoggingIn || entry.state == RTG::RtgHelperState::WorldIdle;
    }

    static uint32 RTG_CountPendingHelpers(uint32 queueType = 0, uint32 bracketId = UINT32_MAX, uint32 team = UINT32_MAX)
    {
        uint32 count = 0;
        RTG::RtgQueueLedger& ledger = RTG::RtgQueueLedger::Instance();
        for (uint32 botId : ledger.GetTrackedBotIds())
        {
            RTG::RtgHelperLedgerEntry const* entry = ledger.Get(botId);
            if (!entry || !RTG_IsTrackedPendingHelperState(*entry))
                continue;

            if (queueType && uint32(entry->target.queueTypeId) != queueType)
                continue;
            if (bracketId != UINT32_MAX && uint32(entry->target.bracketId) != bracketId)
                continue;
            if (team != UINT32_MAX && uint32(entry->target.preferredTeam) != team)
                continue;

            ++count;
        }
        return count;
    }

    static bool RTG_IsOwnerInBattlegroundMap(uint32 ownerGuid)
    {
        if (!ownerGuid)
            return false;

        Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
        if (!owner)
            return false;

        return owner->InBattleground() || owner->InArena();
    }

    static bool RTG_DispatchImmediateBgQueueJoin(Player* bot, uint32 desiredQueueType, char const* reason)
    {
        if (!bot || !desiredQueueType || desiredQueueType >= MAX_BATTLEGROUND_QUEUE_TYPES)
            return false;

        if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueueForBattlegroundQueueType(BattlegroundQueueTypeId(desiredQueueType)) ||
            bot->IsInvitedForBattlegroundInstance())
            return true;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        botAI->GetAiObjectContext()->GetValue<uint32>("bg type")->Set(desiredQueueType);
        bool queued = botAI->DoSpecificAction("bg join", Event(), true);
        RTG_RuntimeBreadcrumb(fmt::format("[RTG][QUEUE][DISPATCH] helper={} queue={} reason={} result={}",
            bot->GetGUID().GetCounter(), desiredQueueType, reason ? reason : "rtg", queued ? 1 : 0));
        return queued;
    }

    static bool RTG_IsQueueSupervisorEvent(std::string const& event)
    {
        return event == "add" || event == "logout" || RTG::HasPrefix(event, "rtg_");
    }

    static bool RTG_ClassCanRole(uint8 cls, uint32 role)
    {
        switch (role)
        {
            case lfg::PLAYER_ROLE_TANK:
                return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_DEATH_KNIGHT;
            case lfg::PLAYER_ROLE_HEALER:
                return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
            case lfg::PLAYER_ROLE_DAMAGE:
                return true;
            default:
                return false;
        }
    }

    static uint32 RTG_DefaultRoleForClass(uint8 cls)
    {
        if (RTG_ClassCanRole(cls, lfg::PLAYER_ROLE_HEALER))
            return lfg::PLAYER_ROLE_HEALER;
        if (RTG_ClassCanRole(cls, lfg::PLAYER_ROLE_TANK))
            return lfg::PLAYER_ROLE_TANK;
        return lfg::PLAYER_ROLE_DAMAGE;
    }

    static uint8 RTG_DefaultSpecTabForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_MAGE: return MAGE_TAB_FROST;
            case CLASS_PALADIN: return PALADIN_TAB_RETRIBUTION;
            case CLASS_PRIEST: return PRIEST_TAB_HOLY;
            case CLASS_WARLOCK: return WARLOCK_TAB_DEMONOLOGY;
            case CLASS_SHAMAN: return SHAMAN_TAB_ELEMENTAL;
            default: return 0;
        }
    }

    static uint32 RTG_RoleForClassSpecTab(uint8 cls, uint8 specTab)
    {
        switch (cls)
        {
            case CLASS_DRUID:
                if (specTab == DRUID_TAB_RESTORATION)
                    return lfg::PLAYER_ROLE_HEALER;
                if (specTab == DRUID_TAB_FERAL)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_PALADIN:
                if (specTab == PALADIN_TAB_HOLY)
                    return lfg::PLAYER_ROLE_HEALER;
                if (specTab == PALADIN_TAB_PROTECTION)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_PRIEST:
                return specTab == PRIEST_TAB_SHADOW ? lfg::PLAYER_ROLE_DAMAGE : lfg::PLAYER_ROLE_HEALER;
            case CLASS_SHAMAN:
                return specTab == SHAMAN_TAB_RESTORATION ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_WARRIOR:
                return specTab == WARRIOR_TAB_PROTECTION ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_DEATH_KNIGHT:
                return specTab == DEATH_KNIGHT_TAB_BLOOD ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
            default:
                return lfg::PLAYER_ROLE_DAMAGE;
        }
    }

    static bool RTG_GetOfflineSpecTab(ObjectGuid::LowType guid, uint8 cls, uint8& specTab)
    {
        specTab = RTG_DefaultSpecTabForClass(cls);

        QueryResult specResult = CharacterDatabase.Query("SELECT activeTalentGroup FROM characters WHERE guid = {}", guid);
        uint8 activeSpec = 0;
        if (specResult)
            activeSpec = specResult->Fetch()[0].Get<uint8>();

        uint32 activeMask = (1u << activeSpec);
        uint32 const* talentTabIds = GetTalentTabPages(cls);
        if (!talentTabIds)
            return true;

        std::map<uint8, uint32> tabs = {{0, 0}, {1, 0}, {2, 0}};
        QueryResult talentResult = CharacterDatabase.Query("SELECT spell, specMask FROM character_talent WHERE guid = {}", guid);
        if (!talentResult)
            return true;

        do
        {
            Field* fields = talentResult->Fetch();
            uint32 spellId = fields[0].Get<uint32>();
            uint8 specMask = fields[1].Get<uint8>();
            if ((activeMask & specMask) == 0)
                continue;

            TalentSpellPos const* talentPos = GetTalentSpellPos(spellId);
            if (!talentPos)
                continue;
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentPos->talent_id);
            if (!talentInfo)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            uint32 rank = spellInfo ? spellInfo->GetRank() : 1u;
            if (talentInfo->TalentTab == talentTabIds[0])
                tabs[0] += rank;
            else if (talentInfo->TalentTab == talentTabIds[1])
                tabs[1] += rank;
            else if (talentInfo->TalentTab == talentTabIds[2])
                tabs[2] += rank;
        } while (talentResult->NextRow());

        if ((tabs[0] + tabs[1] + tabs[2]) == 0)
            return true;

        specTab = 0;
        uint32 maxPoints = tabs[0];
        for (uint8 i = 1; i < 3; ++i)
        {
            if (tabs[i] > maxPoints)
            {
                maxPoints = tabs[i];
                specTab = i;
            }
        }
        return true;
    }

    static uint32 RTG_GetOfflineSpecRole(ObjectGuid::LowType guid, uint8 cls)
    {
        uint8 specTab = 0;
        RTG_GetOfflineSpecTab(guid, cls, specTab);
        return RTG_RoleForClassSpecTab(cls, specTab);
    }

    static uint32 RTG_GetActualSpecRole(Player* bot)
    {
        if (!bot)
            return lfg::PLAYER_ROLE_DAMAGE;

        return RTG_RoleForClassSpecTab(bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
    }
    static uint32 RTG_NormalizeQueuedRoleMask(uint32 roleMask)
	{
		if (roleMask & lfg::PLAYER_ROLE_TANK)
			return lfg::PLAYER_ROLE_TANK;
		if (roleMask & lfg::PLAYER_ROLE_HEALER)
			return lfg::PLAYER_ROLE_HEALER;
		if (roleMask & lfg::PLAYER_ROLE_DAMAGE)
			return lfg::PLAYER_ROLE_DAMAGE;
		return 0u;
	}

	static uint32 RTG_TargetLfgRoleCount(uint32 role)
	{
		switch (role)
		{
			case lfg::PLAYER_ROLE_TANK: return 1u;
			case lfg::PLAYER_ROLE_HEALER: return 1u;
			case lfg::PLAYER_ROLE_DAMAGE: return 3u;
			default: return 0u;
		}
	}

	static uint32 RTG_ActualRoleForBot(Player* bot)
	{
		return RTG_GetActualSpecRole(bot);
	}

    static bool RTG_IsRealPlayer(Player* player)
    {
        return player && !GET_PLAYERBOT_AI(player);
    }

    static bool RTG_GroupHasRealPlayer(Group* group)
    {
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (RTG_IsRealPlayer(member))
                return true;
        }

        return false;
    }

    static std::string RTG_MakeBgDemandKey(uint32 queueType, uint32 bracketId)
    {
        return "rtg_bg_real_demand:" + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static std::string RTG_MakeBgTeamNeedKey(uint32 queueType, uint32 bracketId, uint32 teamId)
    {
        return "rtg_bg_team_need:" + std::to_string(queueType) + ":" + std::to_string(bracketId) + ":" + std::to_string(teamId);
    }

    static std::string RTG_MakeBgPhaseKey(uint32 queueType, uint32 bracketId)
    {
        return "rtg_bg_phase:" + std::to_string(queueType) + ":" + std::to_string(bracketId);
    }

    static void RTG_ClearQueueDebuffs(Player* bot)
    {
        if (!bot)
            return;

        bot->RemoveAura(26013); // Deserter
        bot->RemoveAura(71041); // Dungeon Deserter
    }

    static void RTG_PrepareBotForLogout(Player* bot)
    {
        if (!bot)
            return;

        RTG_ClearQueueDebuffs(bot);

        if (bot->isDead())
        {
            bot->ResurrectPlayer(1.0f, false);
            bot->SpawnCorpseBones();
            bot->SetFullHealth();
        }
    }

    static bool RTG_GetBgQueueContext(BattlegroundQueueTypeId queueTypeId, uint32 level, BattlegroundBracketId& bracketId,
                                      uint32& minLevel, uint32& maxLevel)
    {
        if (queueTypeId <= BATTLEGROUND_QUEUE_NONE || queueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
            return false;

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        if (bgTypeId == BATTLEGROUND_TYPE_NONE)
            return false;

        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            return false;

        PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), level);
        if (!pvpDiff)
            return false;

        bracketId = pvpDiff->GetBracketId();
        if (bracketId < BG_BRACKET_ID_FIRST || bracketId >= MAX_BATTLEGROUND_BRACKETS)
            return false;

        minLevel = pvpDiff->minLevel;
        maxLevel = pvpDiff->maxLevel;
        return true;
    }

    static uint64 RTG_MakeBgParticipantKey(uint32 queueTypeId, BattlegroundBracketId bracketId, ObjectGuid guid, bool isBot)
    {
        return (uint64(isBot ? 1u : 0u) << 63) |
               (uint64(queueTypeId & 0x7FFFu) << 48) |
               (uint64(uint32(bracketId) & 0xFFFFu) << 32) |
               uint64(guid.GetCounter());
    }

    static void RTG_RecordBgTeamCounts(BattlegroundInfo& bgInfo, TeamId teamId, bool isBot, bool queueState, bool activeState)
    {
        if (queueState)
        {
            if (isBot)
            {
                if (teamId == TEAM_ALLIANCE)
                    ++bgInfo.bgQueueAllianceBotCount;
                else
                    ++bgInfo.bgQueueHordeBotCount;
            }
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    ++bgInfo.bgQueueAlliancePlayerCount;
                else
                    ++bgInfo.bgQueueHordePlayerCount;
            }
        }

        if (activeState)
        {
            if (isBot)
            {
                if (teamId == TEAM_ALLIANCE)
                    ++bgInfo.bgActiveAllianceBotCount;
                else
                    ++bgInfo.bgActiveHordeBotCount;
            }
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    ++bgInfo.bgActiveAlliancePlayerCount;
                else
                    ++bgInfo.bgActiveHordePlayerCount;
            }
        }
    }

    static bool RTG_IsBgLifecycleOwned(Player* bot, uint32 desiredQueueType)
    {
        if (!bot)
            return false;

        if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue() || bot->IsInvitedForBattlegroundInstance())
            return true;

        if (Group* group = bot->GetGroup())
        {
            if (group->isBGGroup())
                return true;
        }

        Map* map = bot->GetMap();
        if (map && map->IsBattlegroundOrArena())
            return true;

        if (desiredQueueType > BATTLEGROUND_QUEUE_NONE && desiredQueueType < MAX_BATTLEGROUND_QUEUE_TYPES)
        {
            if (bot->InBattlegroundQueueForBattlegroundQueueType(BattlegroundQueueTypeId(desiredQueueType)))
                return true;

            BattlegroundTypeId desiredBgType = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(desiredQueueType));
            if (desiredBgType != BATTLEGROUND_TYPE_NONE && bot->InBattleground() && bot->GetBattlegroundTypeId() == desiredBgType)
                return true;
        }

        return false;
    }

    static BattlegroundQueueTypeId RTG_FindBotQueueTypeForLeave(Player* bot)
    {
        if (!bot)
            return BATTLEGROUND_QUEUE_NONE;

        for (uint8 queueSlot = 0; queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueSlot)
        {
            BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(queueSlot);
            if (queueTypeId > BATTLEGROUND_QUEUE_NONE && queueTypeId < MAX_BATTLEGROUND_QUEUE_TYPES)
                return queueTypeId;
        }

        uint32 desiredTeam = 0;
        uint32 desiredLevel = 0;
        uint32 desiredQueueType = 0;
        if (RTG::ParseBgAddData(sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add"), desiredTeam, desiredLevel, desiredQueueType) &&
            desiredQueueType > BATTLEGROUND_QUEUE_NONE && desiredQueueType < MAX_BATTLEGROUND_QUEUE_TYPES)
            return BattlegroundQueueTypeId(desiredQueueType);

        return BATTLEGROUND_QUEUE_NONE;
    }
}

struct GuidClassRaceInfo
{
    ObjectGuid::LowType guid;
    uint32 rClass;
    uint32 rRace;
};

enum class CityId : uint8 {
    STORMWIND, IRONFORGE, DARNASSUS, EXODAR,
    ORGRIMMAR, UNDERCITY, THUNDER_BLUFF, SILVERMOON_CITY,
    SHATTRATH_CITY, DALARAN
};

enum class FactionId : uint8 { ALLIANCE, HORDE, NEUTRAL };

// Map of banker entry → city + faction
static const std::unordered_map<uint16, std::pair<CityId, FactionId>> bankerToCity = {
    {2455,  {CityId::STORMWIND,       FactionId::ALLIANCE}}, {2456,  {CityId::STORMWIND,       FactionId::ALLIANCE}}, {2457,  {CityId::STORMWIND,       FactionId::ALLIANCE}},
    {2460,  {CityId::IRONFORGE,       FactionId::ALLIANCE}}, {2461,  {CityId::IRONFORGE,       FactionId::ALLIANCE}}, {5099,  {CityId::IRONFORGE,       FactionId::ALLIANCE}},
    {4155,  {CityId::DARNASSUS,       FactionId::ALLIANCE}}, {4208,  {CityId::DARNASSUS,       FactionId::ALLIANCE}}, {4209,  {CityId::DARNASSUS,       FactionId::ALLIANCE}},
    {17773, {CityId::EXODAR,          FactionId::ALLIANCE}}, {18350, {CityId::EXODAR,          FactionId::ALLIANCE}}, {16710, {CityId::EXODAR,          FactionId::ALLIANCE}},
    {3320,  {CityId::ORGRIMMAR,       FactionId::HORDE}},    {3309,  {CityId::ORGRIMMAR,       FactionId::HORDE}},    {3318,  {CityId::ORGRIMMAR,       FactionId::HORDE}},
    {4549,  {CityId::UNDERCITY,       FactionId::HORDE}},    {2459,  {CityId::UNDERCITY,       FactionId::HORDE}},    {2458,  {CityId::UNDERCITY,       FactionId::HORDE}},    {4550, {CityId::UNDERCITY, FactionId::HORDE}},
    {2996,  {CityId::THUNDER_BLUFF,   FactionId::HORDE}},    {8356,  {CityId::THUNDER_BLUFF,   FactionId::HORDE}},    {8357,  {CityId::THUNDER_BLUFF,   FactionId::HORDE}},
    {17631, {CityId::SILVERMOON_CITY, FactionId::HORDE}},    {17632, {CityId::SILVERMOON_CITY, FactionId::HORDE}},    {17633, {CityId::SILVERMOON_CITY, FactionId::HORDE}},
    {16615, {CityId::SILVERMOON_CITY, FactionId::HORDE}},    {16616, {CityId::SILVERMOON_CITY, FactionId::HORDE}},    {16617, {CityId::SILVERMOON_CITY, FactionId::HORDE}},
    {19246, {CityId::SHATTRATH_CITY,  FactionId::NEUTRAL}},  {19338, {CityId::SHATTRATH_CITY,  FactionId::NEUTRAL}},
    {19034, {CityId::SHATTRATH_CITY,  FactionId::NEUTRAL}},  {19318, {CityId::SHATTRATH_CITY,  FactionId::NEUTRAL}},
    {30604, {CityId::DALARAN,         FactionId::NEUTRAL}},  {30605, {CityId::DALARAN,         FactionId::NEUTRAL}},  {30607, {CityId::DALARAN,         FactionId::NEUTRAL}},
    {28675, {CityId::DALARAN,         FactionId::NEUTRAL}},  {28676, {CityId::DALARAN,         FactionId::NEUTRAL}},  {28677, {CityId::DALARAN,         FactionId::NEUTRAL}}
};

// Map of city → available banker entries
static const std::unordered_map<CityId, std::vector<uint16>> cityToBankers = {
    {CityId::STORMWIND,       {2455, 2456, 2457}},
    {CityId::IRONFORGE,       {2460, 2461, 5099}},
    {CityId::DARNASSUS,       {4155, 4208, 4209}},
    {CityId::EXODAR,          {17773, 18350, 16710}},
    {CityId::ORGRIMMAR,       {3320, 3309, 3318}},
    {CityId::UNDERCITY,       {4549, 2459, 2458, 4550}},
    {CityId::THUNDER_BLUFF,   {2996, 8356, 8357}},
    {CityId::SILVERMOON_CITY, {17631, 17632, 17633, 16615, 16616, 16617}},
    {CityId::SHATTRATH_CITY,  {19246, 19338, 19034, 19318}},
    {CityId::DALARAN,         {30604, 30605, 30607, 28675, 28676, 28677, 29530}}
};

// Quick lookup map: banker entry → location
static std::unordered_map<uint32, WorldLocation> bankerEntryToLocation;

void PrintStatsThread() { sRandomPlayerbotMgr.PrintStats(); }

void activatePrintStatsThread()
{
    std::thread t(PrintStatsThread);
    t.detach();
}

void CheckBgQueueThread() { sRandomPlayerbotMgr.CheckBgQueue(); }

void activateCheckBgQueueThread()
{
    std::thread t(CheckBgQueueThread);
    t.detach();
}

void CheckLfgQueueThread() { sRandomPlayerbotMgr.CheckLfgQueue(); }

void activateCheckLfgQueueThread()
{
    std::thread t(CheckLfgQueueThread);
    t.detach();
}

void CheckPlayersThread() { sRandomPlayerbotMgr.CheckPlayers(); }

void activateCheckPlayersThread()
{
    std::thread t(CheckPlayersThread);
    t.detach();
}

class botPIDImpl
{
public:
    botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd);
    ~botPIDImpl();
    double calculate(double setpoint, double pv);
    void adjust(double Kp, double Ki, double Kd)
    {
        _Kp = Kp;
        _Ki = Ki;
        _Kd = Kd;
    }
    void reset() { _integral = 0; }

private:
    double _dt;
    double _max;
    double _min;
    double _Kp;
    double _Ki;
    double _Kd;
    double _pre_error;
    double _integral;
};

botPID::botPID(double dt, double max, double min, double Kp, double Ki, double Kd)
{
    pimpl = new botPIDImpl(dt, max, min, Kp, Ki, Kd);
}
void botPID::adjust(double Kp, double Ki, double Kd) { pimpl->adjust(Kp, Ki, Kd); }
void botPID::reset() { pimpl->reset(); }
double botPID::calculate(double setpoint, double pv) { return pimpl->calculate(setpoint, pv); }
botPID::~botPID() { delete pimpl; }

/**
 * Implementation
 */
botPIDImpl::botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd)
    : _dt(dt), _max(max), _min(min), _Kp(Kp), _Ki(Ki), _Kd(Kd), _pre_error(0), _integral(0)
{
}

double botPIDImpl::calculate(double setpoint, double pv)
{
    // Calculate error
    double error = setpoint - pv;

    // Proportional term
    double Pout = _Kp * error;

    // Integral term
    _integral += error * _dt;
    double Iout = _Ki * _integral;

    // Derivative term
    double derivative = (error - _pre_error) / _dt;
    double Dout = _Kd * derivative;

    // Calculate total output
    double output = Pout + Iout + Dout;

    // Restrict to max/min
    if (output > _max)
    {
        output = _max;
        _integral -= error * _dt;  // Stop integral buildup at max
    }
    else if (output < _min)
    {
        output = _min;
        _integral -= error * _dt;  // Stop integral buildup at min
    }

    // Save error to previous error
    _pre_error = error;

    return output;
}

botPIDImpl::~botPIDImpl() {}

RandomPlayerbotMgr::RandomPlayerbotMgr() : PlayerbotHolder(), processTicks(0)
{
    playersLevel = sPlayerbotAIConfig.randombotStartingLevel;

    if (sPlayerbotAIConfig.enabled || sPlayerbotAIConfig.randomBotAutologin)
    {
        sPlayerbotCommandServer.Start();
    }

    BattlegroundData.clear();  // Clear here and here only.

    // Cleanup on server start: orphaned pet data that's often left behind by bot pets that no longer exist in the DB
    CharacterDatabase.Execute("DELETE FROM pet_aura WHERE guid NOT IN (SELECT id FROM character_pet)");
    CharacterDatabase.Execute("DELETE FROM pet_spell WHERE guid NOT IN (SELECT id FROM character_pet)");
    CharacterDatabase.Execute("DELETE FROM pet_spell_cooldown WHERE guid NOT IN (SELECT id FROM character_pet)");

    for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
    {
        for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            BattlegroundData[queueType][bracket] = BattlegroundInfo();
        }
    }
    BgCheckTimer = 0;
    LfgCheckTimer = 0;
    PlayersCheckTimer = 0;
}

uint32 RandomPlayerbotMgr::GetOnlineRealPlayerCount() const
{
    uint32 count = 0;

    for (auto const& sessionPair : sWorldSessionMgr->GetAllSessions())
    {
        WorldSession* session = sessionPair.second;
        if (!session)
            continue;

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            continue;

        // Exclude all bot accounts
        if (sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player))
		continue;


        ++count;
    }

    return count;
}

bool RandomPlayerbotMgr::RTG_RequestSafeBotLogout(ObjectGuid guid, char const* reason, bool clearQueueState)
{
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return false;

    WorldSession* session = bot->GetSession();
    if (!session)
        return false;

    uint32 botId = guid.GetCounter();
    if (GetEventValue(botId, "logout"))
        return false;

    if (session->isLogingOut() || bot->IsDuringRemoveFromWorld())
    {
        SetEventValue(botId, "logout", 1, 60);
        return false;
    }

    if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
    {
        RTG::RtgQueueLedger& ledger = RTG::RtgQueueLedger::Instance();
        if (ledger.Has(botId))
        {
            RTG::RtgLifecycleResult lifecycle = RTG::EvaluateRetire(bot, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds);
            if (lifecycle.decision != RTG::RtgLifecycleDecision::Allow)
            {
                ledger.RequestRetire(botId, reason ? reason : "rtg");
                SetEventValue(botId, "rtg_bg_retire_when_safe", 1, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds + 30, GetEventData(botId, "add"));
                if (RTG_QueueOwnershipDebugEnabled())
                    LOG_INFO("playerbots", "[RTGDBG][OWNERSHIP] helper={} retire delayed reason='{}' detail='{}'", botId, reason ? reason : "rtg", lifecycle.reason);
                return false;
            }
            ledger.Release(botId, reason ? reason : "rtg");
        }
    }

    if (clearQueueState)
    {
        SetEventValue(botId, "add", 0, 0);
        SetEventValue(botId, "rtg_lfg_pending", 0, 0);
        SetEventValue(botId, "rtg_bg_pending", 0, 0);
        SetEventValue(botId, "rtg_bg_retire_when_safe", 0, 0);
        SetEventValue(botId, "rtg_add_requested", 0, 0);
    }

    currentBots.remove(botId);
    RTG_PrepareBotForLogout(bot);
    SetEventValue(botId, "logout", 1, 60);

    if (RTG_QueueDebugEnabled())
        LOG_INFO("playerbots", "[RTGDBG][LOGOUT] request bot={} reason={} addData='{}'", botId, reason ? reason : "rtg", GetEventData(botId, "add"));
    RTG_RuntimeBreadcrumb(fmt::format("[RTG][LOGOUT] request helper={} reason={}", botId, reason ? reason : "rtg"));

    LogoutPlayerBot(guid);
    return true;
}


void RandomPlayerbotMgr::RTG_RunQueueOwnershipAudit()
{
    if (!sPlayerbotAIConfig.rtgEventDriven || !sPlayerbotAIConfig.rtgQueueOwnershipEnable)
        return;

    RTG::RtgQueueLedger& ledger = RTG::RtgQueueLedger::Instance();
    uint32 maxTransitionMs = std::max<uint32>(5u, sPlayerbotAIConfig.rtgQueueOwnershipMaxTransitionSeconds) * IN_MILLISECONDS;
    uint32 retireRetryTtl = sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds + 30;

    for (ObjectGuid::LowType botId : ledger.GetTrackedBotIds())
    {
        Player* bot = GetPlayerBot(botId);
        RTG::RtgHelperLedgerEntry* entry = ledger.Get(botId);
        if (!entry)
            continue;

        if (!bot || !bot->IsInWorld())
        {
            if (entry->state == RTG::RtgHelperState::Retired)
                ledger.Remove(botId);
            continue;
        }

        if (entry->target.queueTypeId != BATTLEGROUND_QUEUE_NONE)
            RTG::SyncBgHelperState(bot, uint32(entry->target.queueTypeId), entry->target.bracketId, nullptr);

        entry = ledger.Get(botId);
        if (!entry)
            continue;

        bool transitionState = entry->state == RTG::RtgHelperState::LoggingIn ||
                               entry->state == RTG::RtgHelperState::Queued ||
                               entry->state == RTG::RtgHelperState::Invited ||
                               entry->state == RTG::RtgHelperState::Releasing;

        uint32 nowMs = RTG::RTG_GetNowMs32();
        if (transitionState && entry->updatedAtMs && nowMs > entry->updatedAtMs && (nowMs - entry->updatedAtMs) >= maxTransitionMs)
        {
            if (!bot->InBattleground() && !bot->InArena() && !bot->InBattlegroundQueue() && !bot->IsInvitedForBattlegroundInstance())
            {
                if (entry->pendingRetire)
                    ledger.MarkState(botId, RTG::RtgHelperState::Releasing, "transition watchdog preserving retire state");
                else
                    ledger.MarkState(botId, RTG::RtgHelperState::WorldIdle, "transition watchdog reset");

                if (!entry->pendingRetire)
                    ledger.ClearOwnership(botId, "transition watchdog clear ownership");

                if (RTG_QueueOwnershipDebugEnabled())
                    LOG_INFO("playerbots", "[RTGDBG][OWNERSHIP] helper={} transition watchdog reset state={} queue={} instance={}",
                             botId, uint32(entry->state), uint32(entry->target.queueTypeId), entry->ownerInstanceId);
            }
        }

        if (!entry->pendingRetire)
            continue;

        if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
            continue;

        RTG::RtgLifecycleResult lifecycle = RTG::EvaluateRetire(bot, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds);
        if (lifecycle.decision != RTG::RtgLifecycleDecision::Allow)
        {
            SetEventValue(botId, "rtg_bg_retire_when_safe", 1, retireRetryTtl, GetEventData(botId, "add"));
            continue;
        }

        RTG_RequestSafeBotLogout(bot->GetGUID(), "rtg_bg_retire_audit");
    }
}

void RandomPlayerbotMgr::SaveBotsPerPlayerToDB(float ratio) const
{
    PlayerbotsDatabase.Execute(
        "UPDATE playerbots_server_settings SET bots_per_player = {} WHERE id = 1",
        ratio);
}

float RandomPlayerbotMgr::LoadSavedBotsPerPlayerFromDB() const
{
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT bots_per_player FROM playerbots_server_settings WHERE id = 1");

    if (!result)
        return 0.0f; // default: no scaling

    Field* fields = result->Fetch();
    float ratio = fields[0].Get<float>();

    // Basic sanitation only.
    // The *effective* upper cap should be driven by AiPlayerbot.MaxRandomBots,
    // so server owners can raise the ratio without hitting hidden ceilings.
    if (ratio < 0.0f)
        ratio = 0.0f;

    return ratio;
}

void RandomPlayerbotMgr::ForceBotCountRecheck()
{
    // These timers gate population changes
    DelayLoginBotsTimer = 0;
    PlayersCheckTimer = 0;

    // IMPORTANT: allow GetBots() to refill/resize immediately
    currentBots.clear();

    // Invalidate cached target
    SetEventValue(0, "bot_count", 0, 1);

    // Optional: invalidate ratio cache too (if you convert it to members later)
    ratioCachedAt = 0;
    ratioCachedValue = 0.0f;
}

uint32 RandomPlayerbotMgr::GetMaxAllowedBotCount()
{
    // ---------------------------------------------------------
    // 1) Ratio-driven target (DB-driven ratio value)
    // ---------------------------------------------------------
    if (sPlayerbotAIConfig.usePlayerCountRatio)
    {
        // Cache DB reads so we don't hammer the DB
        
time_t now = time(nullptr);
uint32 ttl = sPlayerbotAIConfig.ratioDbCacheSeconds ? sPlayerbotAIConfig.ratioDbCacheSeconds : 10;

if (!ratioCachedAt || (now - ratioCachedAt) >= static_cast<time_t>(ttl))
{
    ratioCachedValue = LoadSavedBotsPerPlayerFromDB();
    ratioCachedAt = now;

    // Optional: mirror into config for visibility / consistency
    sPlayerbotAIConfig.botsPerPlayer = ratioCachedValue;
}

uint32 realPlayers = GetOnlineRealPlayerCount(); // excludes rndbot accounts

double ratio = static_cast<double>(ratioCachedValue);
if (ratio < 0.0)
            ratio = 0.0;

        // Compute desired count with overflow-safe clamping.
        // We do NOT impose a hidden "max bots per player" ceiling here.
        // The only gameplay cap is AiPlayerbot.MaxRandomBots.
        double desiredD = static_cast<double>(realPlayers) * ratio;
        if (!std::isfinite(desiredD))
            desiredD = static_cast<double>(sPlayerbotAIConfig.maxRandomBots);
        uint32 hardMax = sPlayerbotAIConfig.maxRandomBots;
        uint32 target = (desiredD >= static_cast<double>(hardMax))
            ? hardMax
            : static_cast<uint32>(std::ceil(desiredD));

        // Clamp to configured hard bounds
        if (target < sPlayerbotAIConfig.minRandomBots) target = sPlayerbotAIConfig.minRandomBots;
        if (target > sPlayerbotAIConfig.maxRandomBots) target = sPlayerbotAIConfig.maxRandomBots;

        // Keep an event value for visibility/consistency (short TTL)
        SetEventValue(0, "bot_count", target, 30);

        return target;
    }

    // ---------------------------------------------------------
    // 2) Stock behavior: randomize "bot_count" over time if missing/out of range
    // ---------------------------------------------------------
    uint32 target = GetEventValue(0, "bot_count");

    if (!target ||
        target < sPlayerbotAIConfig.minRandomBots ||
        target > sPlayerbotAIConfig.maxRandomBots)
    {
        target = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);

        SetEventValue(0, "bot_count", target,
            urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval,
                  sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    return target;
}

uint32 RandomPlayerbotMgr::GetCommunityLevelCap()  // NOTE: GetCommunityLevelCap is called from the world update thread only.
{
    if (!sPlayerbotAIConfig.communityLevelCapEnabled)
        return 0;

    uint32 cacheSeconds = sPlayerbotAIConfig.communityLevelCapCacheSeconds ? sPlayerbotAIConfig.communityLevelCapCacheSeconds : 60;
    time_t now = time(nullptr);

    if (communityLevelCapCachedAt && (now - communityLevelCapCachedAt) < static_cast<time_t>(cacheSeconds))
        return communityLevelCapCachedValue;

    std::vector<uint32> levels;
    levels.reserve(64);

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    HashMapHolder<Player>::MapType const& m = ObjectAccessor::GetPlayers();

    for (auto const& it : m)
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld())
            continue;

        // Exclude bots (random bots / addclass bots)
        if (IsRandomBot(p) || IsAddclassBot(p))
            continue;

        // Exclude any AI-controlled non-real players just in case
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
        {
            if (!ai->IsRealPlayer())
                continue;
        }

        levels.push_back(p->GetLevel());
    }

    if (levels.empty())
    {
        communityLevelCapCachedAt = now;
        communityLevelCapCachedValue = 0;
        return 0;
    }

    std::sort(levels.begin(), levels.end(), std::greater<uint32>());

    uint32 topN = sPlayerbotAIConfig.communityLevelCapTopN ? sPlayerbotAIConfig.communityLevelCapTopN : 20;
    if (topN > levels.size())
        topN = static_cast<uint32>(levels.size());

    uint64 sum = 0;
    for (uint32 i = 0; i < topN; ++i)
        sum += levels[i];

    double avg = static_cast<double>(sum) / static_cast<double>(topN);
    int32 cap = static_cast<int32>(std::llround(avg)) + sPlayerbotAIConfig.communityLevelCapBuffer;

    int32 minLvl = std::max<int32>(sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL), sPlayerbotAIConfig.randomBotMinLevel);
    int32 maxLvl = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    if (cap < minLvl) cap = minLvl;
    if (cap > maxLvl) cap = maxLvl;

    communityLevelCapCachedAt = now;
    communityLevelCapCachedValue = static_cast<uint32>(cap);
    return communityLevelCapCachedValue;
}


void RandomPlayerbotMgr::LogPlayerLocation()
{
    activeBots = 0;

    try
    {
        sPlayerbotAIConfig.openLog("player_location.csv", "w");

        if (sPlayerbotAIConfig.randomBotAutologin)
        {
            for (auto i : GetAllBots())
            {
                Player* bot = i.second;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "RND"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",err,err,err,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }

            for (auto i : GetPlayers())
            {
                Player* bot = i;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "PLR"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",player,player,player,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }
        }
    }
    catch (...)
    {
        return;
        // This is to prevent some thread-unsafeness. Crashes would happen if bots get added or removed.
        // We really don't care here. Just skip a log. Making this thread-safe is not worth the effort.
    }
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 elapsed, bool /*minimal*/)
{
    if (totalPmo)
        totalPmo->finish();

    totalPmo = sPerfMonitor.start(PERF_MON_TOTAL, "RandomPlayerbotMgr::FullTick");

    if (!sPlayerbotAIConfig.enabled)
        return;

    // RTG standalone queue-helper mode intentionally runs with
    // RandomBotAutologin disabled. Do not short-circuit the full tick in
    // that mode or queue demand/acquisition/logging will never execute.
    if (!sPlayerbotAIConfig.randomBotAutologin && !sPlayerbotAIConfig.rtgEventDriven)
        return;

    // Enforce community level cap as a hard XP ceiling for randombots.
    // This prevents bots from leveling past the current community cap even if they gain XP in the world.
    if (sPlayerbotAIConfig.communityLevelCapEnabled)
    {
        uint32 cap = GetCommunityLevelCap();
        if (cap > 0)
        {
            for (auto const& it : playerBots)
            {
                Player* bot = it.second;
                if (!bot || !bot->IsInWorld() || !IsRandomBot(bot))
                    continue;

                if (bot->GetLevel() >= cap)
                    bot->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
                else
                    bot->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            }
        }
    }


    /*if (sPlayerbotAIConfig.enablePrototypePerformanceDiff)
    {
        LOG_INFO("playerbots", "---------------------------------------");
        LOG_INFO("playerbots",
                 "PROTOTYPE: Playerbot performance enhancements are active. Issues and instability may occur.");
        LOG_INFO("playerbots", "---------------------------------------");
        ScaleBotActivity();
    }*/

    uint32 maxAllowedBotCount = GetMaxAllowedBotCount();

	// If ratio scaling is enabled, override target bot count dynamically
	if (sPlayerbotAIConfig.usePlayerCountRatio)
	{
		uint32 maxAllowedBotCount = GetMaxAllowedBotCount();

		// Optional: store into event cache so .playerbots rndbot stats can show it
		// Keep TTL short so it tracks changes
		SetEventValue(0, "bot_count_ratio_target", maxAllowedBotCount, 30);
	}
	else
	{
		// Stock behavior: randomize target bot_count over time if invalid/out of range
		maxAllowedBotCount = GetEventValue(0, "bot_count");
		
		if (!maxAllowedBotCount || (maxAllowedBotCount < sPlayerbotAIConfig.minRandomBots ||
									maxAllowedBotCount > sPlayerbotAIConfig.maxRandomBots))
		{
			maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
			SetEventValue(0, "bot_count", maxAllowedBotCount,
						urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval,
								sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
		}
	}

    // ------------------------------------------------------------------
    // RTG: queue-driven population target
    // LFG and BG assistance are tracked independently, but share the same
    // temporary bot budget so idle world bots stay out of the way.
    // ------------------------------------------------------------------
    bool rtgLfgDemand = false;
    bool rtgBgDemand = false;
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 baseWorld = sPlayerbotAIConfig.rtgKeepWorldBots ? maxAllowedBotCount : 0u;
        uint32 lfgStart = GetEventValue(0, "rtg_lfg_start");
        uint32 lfgNeed = GetEventValue(0, "rtg_lfg_need_total");
        uint32 bgStart = GetEventValue(0, "rtg_bg_start");
        uint32 bgNeed = GetEventValue(0, "rtg_bg_need_total");

        rtgLfgDemand = (lfgStart != 0) && (lfgNeed != 0);
        rtgBgDemand = (bgStart != 0) && (bgNeed != 0);

        uint32 trackedManaged = 0;
        for (uint32 botId : currentBots)
        {
            if (!GetEventValue(botId, "add"))
                continue;

            std::string addData = GetEventData(botId, "add");
            if (!RTG::IsQueueManagedAddData(addData))
                continue;

            ++trackedManaged;
        }

        uint32 cappedLfgNeed = std::min<uint32>(lfgNeed, sPlayerbotAIConfig.rtgLfgMaxBots);
        uint32 cappedBgNeed = std::min<uint32>(bgNeed, sPlayerbotAIConfig.rtgBgMaxBots);
        uint32 unresolvedNeed = std::min<uint32>(cappedLfgNeed + cappedBgNeed, sPlayerbotAIConfig.rtgEventMaxBots);
        uint32 eventTarget = std::min<uint32>(trackedManaged + unresolvedNeed, sPlayerbotAIConfig.rtgEventMaxBots);
        maxAllowedBotCount = baseWorld + eventTarget;

        if (!rtgLfgDemand && !rtgBgDemand && !sPlayerbotAIConfig.rtgKeepWorldBots)
            maxAllowedBotCount = 0;

        SetEventValue(0, "rtg_target", maxAllowedBotCount, std::max<uint32>(30u, sPlayerbotAIConfig.rtgQueueGraceSeconds + 120));

        if (RTG_QueueDebugEnabled())
        {
            LOG_INFO("playerbots", "[RTG][CONTROL][TARGET] trackedManaged={} unresolvedNeed={} lfgNeed={} bgNeed={} cappedLfgNeed={} cappedBgNeed={} eventTarget={} baseWorld={} maxAllowed={}",
                trackedManaged, unresolvedNeed, lfgNeed, bgNeed, cappedLfgNeed, cappedBgNeed, eventTarget, baseWorld, maxAllowedBotCount);
        }

        static bool rtgStandaloneLogged = false;
        if (!rtgStandaloneLogged && !sPlayerbotAIConfig.randomBotAutologin)
        {
            RTG_RuntimeBreadcrumb(fmt::format("[RTG][CONTROL] standalone queue-helper control active (RTGMaxBots={}, AccountCount={})",
                RTG_GetStandaloneHelperCeiling(), sPlayerbotAIConfig.randomBotAccountCount));
            rtgStandaloneLogged = true;
        }
    }

    GetBots();

    // ------------------------------------------------------------------
    // RTG dungeon-session pinning
    // Once an RTG LFG bot has successfully transitioned into a dungeon run,
    // keep it protected from normal queue cleanup. This prevents wipes or
    // transient regrouping states from causing the bot to leave party/logout.
    // ------------------------------------------------------------------
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 now = NowSeconds();
        for (auto const& kv : playerBots)
        {
            Player* bot = kv.second;
            if (!bot || !bot->IsInWorld())
                continue;

            uint32 botId = kv.first.GetCounter();
            std::string addData = GetEventData(botId, "add");
            if (!RTG::HasPrefix(addData, "rtg_lfg:"))
                continue;

            Map* map = bot->GetMap();
            Group* group = bot->GetGroup();
            lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
            bool inDungeonRun = (map && (map->IsDungeon() || map->IsRaid())) ||
                                (group && group->isLFGGroup()) ||
                                (state == lfg::LFG_STATE_DUNGEON);

            if (inDungeonRun)
                SetEventValue(botId, "rtg_dungeon_active", now, 7200);
        }
    }

    // ------------------------------------------------------------------
    // RTG policy: bots must not remain grouped in the open world.
    // - Allow groups ONLY while inside Dungeon Finder instances (LFG groups)
    //   or inside Battleground/Arena (BG groups).
    // - Dissolve bot-only groups (no real players online) even in allowed
    //   contexts to prevent exploitation and "bot shields".
    // ------------------------------------------------------------------
    {
        uint32 now = NowSeconds();

        for (auto const& kv : playerBots)
        {
            Player* bot = kv.second;
            if (!bot || !bot->IsInWorld() || !IsRandomBot(bot))
                continue;

            uint32 botId = bot->GetGUID().GetCounter();
            bool rtgDungeonActive = sPlayerbotAIConfig.rtgEventDriven && GetEventValue(botId, "rtg_dungeon_active");
            Group* group = bot->GetGroup();
            if (!group)
            {
                if (rtgDungeonActive)
                    continue;
                SetEventValue(botId, "rtg_group_noreal", 0, 0);
                continue;
            }

            Map* map = bot->GetMap();
            bool inBg = map && map->IsBattlegroundOrArena();
            bool inInstance = map && (map->IsDungeon() || map->IsRaid());

            bool isBgGroup = group->isBGGroup();
            bool isLfgGroup = group->isLFGGroup();

            bool allowedHere = (isBgGroup && inBg) || (isLfgGroup && inInstance);

            // Any non-LFG / non-BG grouping is forbidden.
            if ((!isBgGroup && !isLfgGroup) || !allowedHere)
            {
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    botAI->LeaveOrDisbandGroup();

                SetEventValue(bot->GetGUID().GetCounter(), "rtg_group_noreal", 0, 0);
                continue;
            }

            // Dissolve bot-only groups (no real players online).
            bool hasRealOnline = false;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                Player* member = ObjectAccessor::FindPlayer(slot.guid);
                if (!member)
                    continue;

                // A "real" player is one without PlayerbotAI attached.
                if (!GET_PLAYERBOT_AI(member))
                {
                    hasRealOnline = true;
                    break;
                }
            }

            if (rtgDungeonActive)
            {
                SetEventValue(botId, "rtg_group_noreal", 0, 0);
                continue;
            }

            // BG groups are owned by battleground lifecycle. Do not force-disband
            // bot-only battleground groups here just because no real player remains.
            // That can tear down live battleground state while the instance is still open.
            if (isBgGroup && inBg)
            {
                SetEventValue(botId, "rtg_group_noreal", 0, 0);
                continue;
            }

            if (hasRealOnline)
            {
                SetEventValue(botId, "rtg_group_noreal", 0, 0);
                continue;
            }

            // Start / continue a short grace period so we don't flail on transient states.
            uint32 started = GetEventValue(botId, "rtg_group_noreal");
            if (!started)
            {
                SetEventValue(botId, "rtg_group_noreal", now, 600);
                continue;
            }

            // Never dissolve battleground / arena / queued battleground helpers here.
            // A bot that is already inside a battleground must remain there until the match
            // naturally ends or dedicated battleground lifecycle logic retires it.
            if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
            {
                SetEventValue(botId, "rtg_group_noreal", 0, 0);
                continue;
            }

            // After 10s with no real players online, dissolve allowed non-BG groups only.
            if (now > started + 10)
            {
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                    botAI->LeaveOrDisbandGroup();

                SetEventValue(botId, "rtg_group_noreal", 0, 0);
            }
        }
    }
    std::list<uint32> availableBots = currentBots;
    uint32 availableBotCount = availableBots.size();
    uint32 onlineBotCount = playerBots.size();

    // ------------------------------------------------------------------
    // RTG: event-driven shrink
    // If event-driven mode is on and we're above target (often 0), mark a few
    // bots for logout quickly so they go offline when not needed.
    // ------------------------------------------------------------------
	if (sPlayerbotAIConfig.rtgEventDriven && onlineBotCount > maxAllowedBotCount)
	{
		uint32 over = onlineBotCount - maxAllowedBotCount;
		uint32 toLogout = std::min<uint32>(over, std::max<uint32>(1u, sPlayerbotAIConfig.randomBotsPerInterval));
		std::vector<ObjectGuid> rtgShrinkLogout;

		for (auto const& kv : playerBots)
		{
			if (!toLogout)
				break;

			ObjectGuid botGuid = kv.first;
			Player* bot = kv.second;
			if (!bot || !bot->IsInWorld())
				continue;

			uint32 botId = botGuid.GetCounter();

			// Never shrink active BG participants
			if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
				continue;

			// Never shrink bots that are actively queued for LFG or already in dungeon state
			lfg::LfgState lfgState = sLFGMgr->GetState(bot->GetGUID());
			if (lfgState != lfg::LFG_STATE_NONE)
				continue;

			if (GetEventValue(botId, "rtg_dungeon_active"))
				continue;

			Group* grp = bot->GetGroup();
			if (grp)
			{
				if (grp->isLFGGroup() || RTG_GroupHasRealPlayer(grp))
				{
					SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
					continue;
				}
			}

			Map* map = bot->GetMap();
			if (map && (map->IsDungeon() || map->IsRaid()))
				continue;

			if (bot->isDead() || bot->GetCorpse())
				continue;

			if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
				continue;

			rtgShrinkLogout.push_back(botGuid);
			--toLogout;
		}

		for (ObjectGuid const& botGuid : rtgShrinkLogout)
			RTG_RequestSafeBotLogout(botGuid, "rtg_shrink");
	}


	// If RTG queue demand has vanished, clear temporary queue-fill bots
	// that are not actually inside a dungeon run or actively queued.
	if (sPlayerbotAIConfig.rtgEventDriven && !rtgLfgDemand && !rtgBgDemand)
	{
		std::vector<ObjectGuid> rtgIdleLogout;
		std::vector<ObjectGuid> rtgStaleQueueBots;

		for (auto const& kv : playerBots)
		{
			ObjectGuid botGuid = kv.first;
			Player* bot = kv.second;
			if (!bot || !bot->IsInWorld())
				continue;

			uint32 botId = botGuid.GetCounter();
			std::string addData = GetEventData(botId, "add");
			if (!RTG::HasPrefix(addData, "rtg_lfg:"))
				continue;

			Map* map = bot->GetMap();
			if (map && (map->IsDungeon() || map->IsRaid()))
			{
				RTG_ClearQueueDebuffs(bot);
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				continue;
			}

			if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
				continue;

			uint32 desiredRole = 0;
			uint32 desiredOwner = 0;
			unsigned int parsedTeam = 0;
			unsigned int parsedLevel = 0;
			unsigned int parsedRole = 0;
			unsigned int parsedOwner = 0;
			if (RTG::ParseLfgAddData(addData, parsedTeam, parsedLevel, &parsedRole, &parsedOwner))
			{
				desiredRole = parsedRole;
				desiredOwner = parsedOwner;
			}

			bool ownerHasRealDemand = desiredOwner && GetEventValue(desiredOwner, "rtg_lfg_real_demand");

			if (desiredRole)
			{
				uint32 actualRole = RTG_ActualRoleForBot(bot);
				if (actualRole != desiredRole)
				{
					rtgStaleQueueBots.push_back(botGuid);
					continue;
				}
			}

			lfg::LfgState botState = sLFGMgr->GetState(bot->GetGUID());

			// If the bot is queued, leave it alone and refresh patience
			if (botState != lfg::LFG_STATE_NONE && botState < lfg::LFG_STATE_DUNGEON)
			{
				SetEventValue(botId, "rtg_lfg_pending", 1, 15, addData);
				continue;
			}

			Group* grp = bot->GetGroup();
			if (grp)
			{
				if (grp->isLFGGroup() || RTG_GroupHasRealPlayer(grp) || ownerHasRealDemand)
				{
					SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
					continue;
				}
			}

			if ((bot->isDead() || bot->GetCorpse()) && ownerHasRealDemand)
			{
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				continue;
			}

			if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
				continue;

			rtgIdleLogout.push_back(botGuid);
		}

		for (ObjectGuid const& guid : rtgStaleQueueBots)
			RTG_RequestSafeBotLogout(guid, "rtg_lfg_role_mismatch");

		for (ObjectGuid const& botGuid : rtgIdleLogout)
			RTG_RequestSafeBotLogout(botGuid, "rtg_lfg_idle");
	}


	// RTG: replace queue-fill bots that made it online but never actually joined LFG.
	// Count only bots that are truly queued (or still within a pending window)
	// toward shortage satisfaction. Stale bots are logged out and replaced.
	if (sPlayerbotAIConfig.rtgEventDriven)
	{
		std::vector<ObjectGuid> rtgStaleQueueBots;

		for (auto const& kv : playerBots)
		{
			ObjectGuid botGuid = kv.first;
			Player* bot = kv.second;
			if (!bot || !bot->IsInWorld())
				continue;

			uint32 botId = botGuid.GetCounter();
			std::string addData = GetEventData(botId, "add");

			uint32 desiredTeam = 0;
			uint32 desiredLevel = 0;
			uint32 desiredRole = 0;
			uint32 desiredOwner = 0;
			if (!RTG::ParseLfgAddData(addData, desiredTeam, desiredLevel, &desiredRole, &desiredOwner))
				continue;

			bool ownerHasRealDemand = desiredOwner && GetEventValue(desiredOwner, "rtg_lfg_real_demand");
            bool ownerInBgMap = RTG_IsOwnerInBattlegroundMap(desiredOwner);

			Map* map = bot->GetMap();
			if (map && (map->IsDungeon() || map->IsRaid()))
			{
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				SetEventValue(botId, "rtg_lfg_pending", 0, 0);
				continue;
			}

            if (ownerInBgMap)
            {
                RTG_RuntimeBreadcrumb(fmt::format("[RTG][LFG][ABANDON] helper={} owner={} reason=owner_in_bg_map", botId, desiredOwner));
                SetEventValue(botId, "add", 0, 0);
                SetEventValue(botId, "rtg_add_requested", 0, 0);
                SetEventValue(botId, "rtg_lfg_pending", 0, 0);
                currentBots.remove(botId);
                rtgStaleQueueBots.push_back(botGuid);
                continue;
            }

			if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
				continue;

			Group* grp = bot->GetGroup();
			if (grp && (grp->isLFGGroup() || RTG_GroupHasRealPlayer(grp) || ownerHasRealDemand))
			{
				RTG_ClearQueueDebuffs(bot);
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				SetEventValue(botId, "rtg_lfg_pending", 0, 0);
				continue;
			}

			if (bot->GetTeamId() != desiredTeam || bot->GetLevel() != desiredLevel)
			{
				SetEventValue(botId, "add", 0, 0);
				SetEventValue(botId, "rtg_lfg_pending", 0, 0);
				currentBots.remove(botId);
				rtgStaleQueueBots.push_back(botGuid);
				continue;
			}

			lfg::LfgState botState = sLFGMgr->GetState(bot->GetGUID());

			// If the bot is actively queued, do not recycle it
			if (botState != lfg::LFG_STATE_NONE && botState < lfg::LFG_STATE_DUNGEON)
			{
				if (desiredRole)
				{
					uint32 actualRoles = sLFGMgr->GetRoles(bot->GetGUID());
					if ((actualRoles & desiredRole) == 0)
					{
						SetEventValue(botId, "add", 0, 0);
						SetEventValue(botId, "rtg_lfg_pending", 0, 0);
						currentBots.remove(botId);
						rtgStaleQueueBots.push_back(botGuid);
						continue;
					}
				}

				SetEventValue(botId, "rtg_lfg_pending", 1, 15, addData);
				continue;
			}

			// Pending bots get more patience
			if (GetEventValue(botId, "rtg_lfg_pending"))
			{
				SetEventValue(botId, "rtg_lfg_pending", 1, 15, addData);
				continue;
			}

			if (grp && (grp->isLFGGroup() || RTG_GroupHasRealPlayer(grp) || ownerHasRealDemand))
			{
				RTG_ClearQueueDebuffs(bot);
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				continue;
			}

			if ((bot->isDead() || bot->GetCorpse()) && ownerHasRealDemand)
			{
				SetEventValue(botId, "rtg_dungeon_active", NowSeconds(), 7200);
				continue;
			}

			if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
				continue;

			rtgStaleQueueBots.push_back(botGuid);
		}

		for (ObjectGuid const& botGuid : rtgStaleQueueBots)
			RTG_RequestSafeBotLogout(botGuid, "rtg_lfg_stale");
	}


    // RTG: cleanup battleground helper bots that are no longer needed.
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        std::vector<ObjectGuid> rtgBgLogout;

        for (auto const& kv : playerBots)
        {
            ObjectGuid botGuid = kv.first;
            Player* bot = kv.second;
            if (!bot || !bot->IsInWorld())
                continue;

            uint32 botId = botGuid.GetCounter();
            std::string addData = GetEventData(botId, "add");
            uint32 desiredTeam = 0;
            uint32 desiredLevel = 0;
            uint32 desiredQueueType = 0;
            if (!RTG::ParseBgAddData(addData, desiredTeam, desiredLevel, desiredQueueType))
                continue;

            bool bgHasRealDemand = false;
            BattlegroundTypeId desiredBgType = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(desiredQueueType));
            if (desiredBgType != BATTLEGROUND_TYPE_NONE)
            {
                if (Battleground* desiredBgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(desiredBgType))
                {
                    if (PvPDifficultyEntry const* desiredBracket = GetBattlegroundBracketByLevel(desiredBgTemplate->GetMapId(), desiredLevel ? desiredLevel : bot->GetLevel()))
                        bgHasRealDemand = GetEventValue(0, RTG_MakeBgDemandKey(uint32(desiredQueueType), uint32(desiredBracket->GetBracketId()))) != 0;
                }
            }

            bool wrongTeam = desiredTeam && bot->GetTeamId() != desiredTeam;
            bool noLongerNeeded = !bgHasRealDemand || !rtgBgDemand || wrongTeam;
            bool lifecycleOwned = RTG_IsBgLifecycleOwned(bot, desiredQueueType);

            if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
            {
                RTG::SyncBgHelperState(bot, desiredQueueType, BG_BRACKET_ID_FIRST, nullptr);
                RTG::RtgQueueLedger::Instance().ClearRetireRequest(botId);
            }

            bool inQueueState = bot->InBattlegroundQueueForBattlegroundQueueType(BattlegroundQueueTypeId(desiredQueueType)) ||
                                bot->InBattleground() || bot->InArena() || bot->IsInvitedForBattlegroundInstance();
            bool queueGrace = GetEventValue(botId, "rtg_bg_queue_grace") != 0;

            if (!noLongerNeeded && !inQueueState)
            {
                SetEventValue(botId, "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                SetEventValue(botId, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);

                if (!GetEventValue(botId, "rtg_bg_queue_retry"))
                {
                    bool queuedNow = RTG_DispatchImmediateBgQueueJoin(bot, desiredQueueType, queueGrace ? "queue_retry" : "login_or_idle");
                    SetEventValue(botId, "rtg_bg_queue_retry", 1, RTG_GetQueueRetryWindowSeconds(), addData);
                    if (queuedNow)
                    {
                        SetEventValue(botId, "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                        SetEventValue(botId, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                    }
                }
            }

            // Layer 2: lifecycle safety. If battleground state still owns this helper,
            // never force-retire it here. Only mark it for retirement after the helper
            // becomes fully detached from battleground / queue / invite / BG-group state.
            if (lifecycleOwned)
            {
                RTG_ClearQueueDebuffs(bot);
                SetEventValue(botId, "rtg_bg_pending", 0, 0);
                SetEventValue(botId, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);

                if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
                {
                    RTG::RtgQueueLedger& ledger = RTG::RtgQueueLedger::Instance();
                    ledger.RequestRetire(botId, noLongerNeeded ? "bg helper pending safe retire" : "bg helper still active");
                    if (!noLongerNeeded)
                        ledger.ClearRetireRequest(botId);
                }

                if (noLongerNeeded)
                    SetEventValue(botId, "rtg_bg_retire_when_safe", 1, 120, addData);
                else
                    SetEventValue(botId, "rtg_bg_retire_when_safe", 0, 0);
                continue;
            }

            if (GetEventValue(botId, "rtg_bg_pending"))
            {
                // Layer 1: demand reservation. As long as demand still exists, keep the
                // helper reserved for a short window instead of aggressively recycling it.
                if (!noLongerNeeded)
                {
                    SetEventValue(botId, "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                    SetEventValue(botId, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                    continue;
                }

                SetEventValue(botId, "rtg_bg_pending", 0, 0);
                SetEventValue(botId, "rtg_bg_queue_grace", 0, 0);
            }

            bool retireWhenSafe = GetEventValue(botId, "rtg_bg_retire_when_safe") != 0;
            bool outOfBgState = !RTG_IsBgLifecycleOwned(bot, desiredQueueType);

            if (outOfBgState && (retireWhenSafe || noLongerNeeded))
            {
                if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
                    continue;

                if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
                {
                    RTG::RtgLifecycleResult lifecycle = RTG::EvaluateRetire(bot, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds);
                    if (lifecycle.decision != RTG::RtgLifecycleDecision::Allow)
                    {
                        SetEventValue(botId, "rtg_bg_retire_when_safe", 1, sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds + 30, addData);
                        if (RTG_QueueOwnershipDebugEnabled())
                            LOG_INFO("playerbots", "[RTGDBG][OWNERSHIP] helper={} delay retire detail='{}'", botId, lifecycle.reason);
                        continue;
                    }
                }

                RTG_RuntimeBreadcrumb(fmt::format("[RTG][RETIRE] allowing helper={} queue={} noLongerNeeded={}", botId, desiredQueueType, noLongerNeeded ? 1 : 0));
                rtgBgLogout.push_back(botGuid);
                continue;
            }

            // Demand still exists and no battleground lifecycle currently owns this helper.
            // Keep it online and reserved so the join action can still satisfy the queue.
            if (!noLongerNeeded)
            {
                SetEventValue(botId, "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                SetEventValue(botId, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                continue;
            }
        }

        for (ObjectGuid const& botGuid : rtgBgLogout)
            RTG_RequestSafeBotLogout(botGuid, "rtg_bg_retire");
    }


	// RTG: cleanup finished/abandoned dungeon-session bots only after they are
	// truly out of the run for a while. This preserves bots through wipes.
	if (sPlayerbotAIConfig.rtgEventDriven)
	{
		uint32 now = NowSeconds();
		std::vector<ObjectGuid> rtgFinishedDungeonLogout;

		for (auto const& kv : playerBots)
		{
			ObjectGuid botGuid = kv.first;
			Player* bot = kv.second;
			if (!bot || !bot->IsInWorld())
				continue;

			uint32 botId = botGuid.GetCounter();
			uint32 activeSince = GetEventValue(botId, "rtg_dungeon_active");
			if (!activeSince)
				continue;

			Map* map = bot->GetMap();
			Group* group = bot->GetGroup();

			bool stillInRun =
				(map && (map->IsDungeon() || map->IsRaid())) ||
				(group && group->isLFGGroup()) ||
				bot->isDead() ||
				bot->GetCorpse() ||
				bot->IsInCombat() ||
				bot->IsBeingTeleported() ||
				bot->HasUnitState(UNIT_STATE_IN_FLIGHT);

			if (stillInRun)
			{
				SetEventValue(botId, "rtg_dungeon_active", now, 7200);
				continue;
			}

			if (now <= activeSince + 300)
				continue;

			SetEventValue(botId, "rtg_dungeon_active", 0, 0);
			rtgFinishedDungeonLogout.push_back(botGuid);
		}

		for (ObjectGuid const& botGuid : rtgFinishedDungeonLogout)
			RTG_RequestSafeBotLogout(botGuid, "rtg_dungeon_finished");
	}

// ---------------------------------------------------------
// Seed replacement: keep open-world population steady even when
// bots are "away" in BG/Arena or dungeon/raid instances.
// These extra bots are temporary: when away-bots return, the
// normal shrink logic will bring totals back down.
// ---------------------------------------------------------
if (sPlayerbotAIConfig.enabled && !sPlayerbotAIConfig.rtgEventDriven) // sanity
{
    uint32 openWorldBotCount = 0;
    uint32 awayBotCount = 0;

    for (auto const& kv : playerBots)
    {
        Player* bot = kv.second;
        if (!bot || !bot->IsInWorld())
            continue;

        Map* map = bot->GetMap();
        if (map && (map->IsBattlegroundOrArena() || map->IsDungeon() || map->IsRaid()))
        {
            ++awayBotCount;
            continue;
        }

        ++openWorldBotCount;
    }

    // We aim for at least `maxAllowedBotCount` bots in the open world.
    // If some bots are away, temporarily raise the overall target so the
    // world still feels populated.
    if (openWorldBotCount < maxAllowedBotCount)
    {
        uint32 deficit = maxAllowedBotCount - openWorldBotCount;

        // Hard safety cap to prevent runaway growth if something goes wrong.
        // This is intentionally conservative and can be tuned later.
        uint32 const maxExtraSeed = 50;
        if (deficit > maxExtraSeed)
            deficit = maxExtraSeed;

        maxAllowedBotCount += deficit;

        if (sPlayerbotAIConfig.debugRatioScaling)
        {
            LOG_INFO("playerbots",
                "[SeedReplacement] openWorld={} away={} baseTarget={} extraSeed={} newTarget={}",
                openWorldBotCount, awayBotCount, (maxAllowedBotCount - deficit), deficit, maxAllowedBotCount);
        }
    }
}

    uint32 onlineBotFocus = 75;
    // When we need to *grow* the bot population, prioritize logins over updates.
    // Shrink remains slower / more conservative by keeping a higher focus on updates.
    if (onlineBotCount < maxAllowedBotCount)
        onlineBotFocus = 25;
    // If we're far below target (e.g. server start), grow even faster.
    if (onlineBotCount + sPlayerbotAIConfig.randomBotsPerInterval < maxAllowedBotCount)
        onlineBotFocus = 10;
    // only keep updating till initializing time has completed,
    // which prevents unneeded expensive GameTime calls.
    if (_isBotInitializing)
    {
        _isBotInitializing = GameTime::GetUptime().count() < RTG_GetStandaloneHelperCeiling() * (0.11 + 0.4);
    }

    uint32 updateIntervalTurboBoost = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
    SetNextCheckDelay(updateIntervalTurboBoost * (onlineBotFocus + 25) * 10);

    PerfMonitorOperation* pmo = sPerfMonitor.start(
        PERF_MON_TOTAL,
        onlineBotCount < maxAllowedBotCount ? "RandomPlayerbotMgr::Login" : "RandomPlayerbotMgr::UpdateAIInternal");

    bool realPlayerIsLogged = false;
    bool allowLoginBotsNow = true;

    // Ratio scaling uses a lightweight cooldown (event cache) so growth can react faster than shrink,
    // without introducing new persistent state.
    bool ratioGrowOk = true;
    bool ratioShrinkOk = true;
    uint32 ratioGrowSeconds   = GetTuningOrDefault("ratio_grow_s",   sPlayerbotAIConfig.ratioGrowCheckSeconds);
	uint32 ratioShrinkSeconds = GetTuningOrDefault("ratio_shrink_s", sPlayerbotAIConfig.ratioShrinkCheckSeconds);
	uint32 ratioMaxShrink     = GetTuningOrDefault("ratio_max_shrink", sPlayerbotAIConfig.ratioMaxShrinkPerCheck);


    if (sPlayerbotAIConfig.usePlayerCountRatio)
    {
        ratioGrowOk = (GetEventValue(0, "ratio_grow_cd") == 0);
        ratioShrinkOk = (GetEventValue(0, "ratio_shrink_cd") == 0);
    }

    if (sPlayerbotAIConfig.disabledWithoutRealPlayer)
    {
        uint32 onlineRealPlayers = GetOnlineRealPlayerCount();
        if (onlineRealPlayers > 0)
        {
            RealPlayerLastTimeSeen = time(nullptr);
            realPlayerIsLogged = true;

            if (DelayLoginBotsTimer == 0)
            {
                DelayLoginBotsTimer = time(nullptr) + sPlayerbotAIConfig.disabledWithoutRealPlayerLoginDelay;
            }
        }
        else
        {
            realPlayerIsLogged = false;

            if (DelayLoginBotsTimer)
            {
                DelayLoginBotsTimer = 0;
            }

            if (sPlayerbotAIConfig.rtgEventDriven)
            {
                SetEventValue(0, "rtg_bg_any_real_queued", 0, 0);
                SetEventValue(0, "rtg_bg_any_real_demand", 0, 0);
                SetEventValue(0, "rtg_bg_need_total", 0, 0);
                SetEventValue(0, "rtg_bg_start", 0, 0);
                SetEventValue(0, "rtg_lfg_need_total", 0, 0);
                SetEventValue(0, "rtg_lfg_start", 0, 0);
            }

            if (RealPlayerLastTimeSeen != 0 && onlineBotCount > 0 &&
                time(nullptr) > RealPlayerLastTimeSeen + sPlayerbotAIConfig.disabledWithoutRealPlayerLogoutDelay)
            {
                LogoutAllBots();
                LOG_INFO("playerbots", "Logout all bots due no real player session.");
            }
        }

        allowLoginBotsNow = (sPlayerbotAIConfig.disabledWithoutRealPlayer == false);

        // If bots are disabled until a real player is online, normally we wait for the configured delay.
        // However, when ratio scaling is enabled and we're under target, we bypass the delay so population can grow promptly.
        if (!allowLoginBotsNow)
        {
            allowLoginBotsNow = realPlayerIsLogged &&
                (DelayLoginBotsTimer == 0 || time(nullptr) >= DelayLoginBotsTimer ||
                 (sPlayerbotAIConfig.usePlayerCountRatio && onlineBotCount < maxAllowedBotCount));
        }

        if (availableBotCount < maxAllowedBotCount && allowLoginBotsNow && ratioGrowOk)
        {
            AddRandomBots();
        }
    }
    else if (availableBotCount < maxAllowedBotCount)
    {
        AddRandomBots();
    }

    // RTG queue-helper acquisition can append newly reserved helpers into currentBots earlier in
    // this same tick via AddRandomBots(). Refresh the dispatch view now so newly acquired BG/LFG
    // helpers are eligible for immediate ProcessBot login instead of being invisible until the next
    // world update, where they often hit the stall watchdog first under multi-queue pressure.
    availableBots = currentBots;
    availableBotCount = availableBots.size();
    onlineBotCount = playerBots.size();

    if (sPlayerbotAIConfig.rtgEventDriven && RTG_QueueDebugEnabled())
    {
        uint32 pendingDispatchable = 0;
        for (uint32 botId : currentBots)
        {
            if (!GetEventValue(botId, "add"))
                continue;

            if (GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(botId)))
                continue;

            std::string addData = GetEventData(botId, "add");
            if (!RTG::IsQueueManagedAddData(addData))
                continue;

            ++pendingDispatchable;
        }

        if (pendingDispatchable)
        {
            LOG_INFO("playerbots", "[RTG][DISPATCH][REFRESH] availableBotCount={} onlineBotCount={} pendingDispatchable={}",
                     availableBotCount, onlineBotCount, pendingDispatchable);
        }
    }

    if (sPlayerbotAIConfig.syncLevelWithPlayers && !players.empty())
    {
        if (time(nullptr) > (PlayersCheckTimer + 60))
            sRandomPlayerbotMgr.CheckPlayers();
    }

	if (sPlayerbotAIConfig.randomBotJoinBG /* && !players.empty()*/)
    {
        if (time(nullptr) > (BgCheckTimer + (sPlayerbotAIConfig.rtgEventDriven ? 5 : 35)))
        {
            sRandomPlayerbotMgr.CheckBgQueue();
            BgCheckTimer = time(nullptr);
        }
    }

    if (sPlayerbotAIConfig.randomBotJoinLfg /* && !players.empty()*/)
    {
        if (time(nullptr) > (LfgCheckTimer + (sPlayerbotAIConfig.rtgEventDriven ? 5 : 30)))
        {
            sRandomPlayerbotMgr.CheckLfgQueue();
            LfgCheckTimer = time(nullptr);
        }
    }

    if (sPlayerbotAIConfig.randomBotAutologin && time(nullptr) > (printStatsTimer + 300))
    {
        if (!printStatsTimer)
        {
            printStatsTimer = time(nullptr);
        }
        else
        {
            sRandomPlayerbotMgr.PrintStats();
            // activatePrintStatsThread();
        }
    }
    // Ratio-based shrink: mark a few bots for logout when we're above target.
    // This keeps the stock "in-world time" mechanism but allows the population to converge.
    if (sPlayerbotAIConfig.usePlayerCountRatio && onlineBotCount > maxAllowedBotCount && ratioShrinkOk)
    {
        uint32 over = onlineBotCount - maxAllowedBotCount;
        uint32 toMark = std::min<uint32>(over, ratioMaxShrink);

// Build a prioritized logout list:
//  - NEVER logout bots that are: in BG/arena, in BG queue, in dungeons/raids, or grouped with real players.
//  - For the rest, prefer logging out "least disruptive" bots first.
auto isGroupWithRealPlayer = [this](Player* bot) -> bool
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        // Any non-randombot member protects the whole group from shrink-logout.
        if (!IsRandomBot(member))
            return true;
    }

    return false;
};

auto isProtectedFromLogout = [&](Player* bot) -> bool
{
    if (!bot)
        return true;

    if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
        return true;

    Map* map = bot->GetMap();
    if (map && (map->IsDungeon() || map->IsRaid()))
        return true;

    if (isGroupWithRealPlayer(bot))
        return true;

    return false;
};

auto getLogoutPriority = [&](Player* bot) -> int
{
    // Lower number = logout sooner.
    if (!bot || !bot->IsInWorld())
        return 0;

    if (bot->IsBeingTeleported() || bot->IsInFlight())
        return 10;

    if (bot->IsInCombat())
        return 80;

    if (bot->GetGroup())
        return 60;

    return 20;
};

	// Choose logout candidates by priority (lowest priority goes first).
	uint32 toLogout = toMark;
	uint32 loggedOut = 0;
	std::vector<std::pair<ObjectGuid, int>> candidates;
candidates.reserve(playerBots.size());

for (auto const& kv : playerBots)
{
	    ObjectGuid botGuid = kv.first;
	    Player* bot = kv.second;

    if (isProtectedFromLogout(bot))
        continue;

	    candidates.emplace_back(botGuid, getLogoutPriority(bot));
}

std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
{
    if (a.second != b.second)
        return a.second < b.second;
	    return a.first.GetCounter() < b.first.GetCounter();
});

for (auto const& c : candidates)
{
    if (loggedOut >= toLogout)
        break;

	    // IMPORTANT: The stock randombot system uses per-bot event values ("add")
	    // plus the currentBots list to decide whether a bot should stay online.
	    // If we only call LogoutPlayerBot() here, the bot may immediately be re-added
	    // on the next tick because its "add" event is still valid and its id is still
	    // present in currentBots.
	    //
	    // Use the RTG-safe path so repeated shrink passes do not re-queue the same logout.
	    ObjectGuid botGuid = c.first;
	    if (RTG_RequestSafeBotLogout(botGuid, "ratio_shrink"))
            ++loggedOut;
}
	
	    if (loggedOut > 0)
	        SetEventValue(0, "ratio_shrink_cd", 1, ratioShrinkSeconds);
    }

    // Allow faster grow when ratio scaling is enabled and we're below target.
    uint32 intervalCap = sPlayerbotAIConfig.randomBotsPerInterval;
    if (sPlayerbotAIConfig.usePlayerCountRatio && onlineBotCount < maxAllowedBotCount)
        intervalCap *= (onlineBotCount == 0 ? 5 : 2);

    uint32 pendingQueuedLogins = 0;
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        for (uint32 botId : currentBots)
        {
            if (!GetEventValue(botId, "add"))
                continue;

            if (GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(botId)))
                continue;

            std::string addData = GetEventData(botId, "add");
            if (!RTG::IsQueueManagedAddData(addData))
                continue;

            ++pendingQueuedLogins;
        }

        if (pendingQueuedLogins)
            intervalCap = std::max(intervalCap, pendingQueuedLogins);
    }

    uint32 updateBots = intervalCap * onlineBotFocus / 100;
    uint32 maxNewBots =
        (onlineBotCount < maxAllowedBotCount && allowLoginBotsNow && ratioGrowOk)
            ? std::min<uint32>(maxAllowedBotCount - onlineBotCount, intervalCap)
            : 0;
    uint32 loginBots = std::min(intervalCap > updateBots ? intervalCap - updateBots : 0u, maxNewBots);

    if (sPlayerbotAIConfig.rtgEventDriven && pendingQueuedLogins)
    {
        // In standalone RTG queue-helper mode, queued helper logins are not optional background
        // work. They are the mechanism that converts already-approved queue demand into live bots.
        // Use the full online headroom whenever pending helpers exist so finish-fill / new-BG / RDF
        // waves do not sit in a reserved-yet-undispatched state until they hit the stall timeout.
        loginBots = maxNewBots;
        if (updateBots + loginBots > intervalCap)
            updateBots = (intervalCap > loginBots) ? (intervalCap - loginBots) : 0u;

        if (RTG_QueueDebugEnabled())
        {
            LOG_INFO("playerbots", "[RTG][DISPATCH][BUDGET] pendingQueuedLogins={} intervalCap={} updateBots={} loginBots={} maxNewBots={} onlineBotCount={} maxAllowed={}",
                     pendingQueuedLogins, intervalCap, updateBots, loginBots, maxNewBots, onlineBotCount, maxAllowedBotCount);
        }
    }

    // Rate-limit target growth checks (shrink is already rate-limited above).
    if (sPlayerbotAIConfig.usePlayerCountRatio && maxNewBots > 0)
        SetEventValue(0, "ratio_grow_cd", 1, ratioGrowSeconds);

    if (sPlayerbotAIConfig.rtgEventDriven && !availableBots.empty())
    {
        auto rtgDispatchPriority = [&](uint32 botId)
        {
            std::tuple<uint32, uint32, uint32, uint32, uint32> fallback(99u, 99u, 99u, UINT32_MAX, botId);

            if (!GetEventValue(botId, "add"))
                return fallback;

            std::string addData = GetEventData(botId, "add");
            uint32 requestTs = GetEventValue(botId, "rtg_add_requested");

            uint32 desiredTeam = 0;
            uint32 desiredLevel = 0;
            uint32 desiredRole = 0;
            uint32 desiredOwner = 0;
            uint32 desiredQueueType = 0;

            if (RTG::ParseLfgAddData(addData, desiredTeam, desiredLevel, &desiredRole, &desiredOwner))
            {
                uint32 ownerHasRealDemand = desiredOwner && GetEventValue(desiredOwner, "rtg_lfg_real_demand") ? 0u : 1u;
                return std::make_tuple(0u, ownerHasRealDemand, 0u, requestTs ? requestTs : UINT32_MAX, botId);
            }

            if (RTG::ParseBgAddData(addData, desiredTeam, desiredLevel, desiredQueueType))
            {
                BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;
                uint32 minLevel = desiredLevel;
                uint32 maxLevel = desiredLevel;
                uint32 phasePriority = 9u;
                if (RTG_GetBgQueueContext(BattlegroundQueueTypeId(desiredQueueType), desiredLevel, bracketId, minLevel, maxLevel))
                {
                    uint32 phase = GetEventValue(0, RTG_MakeBgPhaseKey(desiredQueueType, uint32(bracketId)));
                    switch (phase)
                    {
                        case 0: phasePriority = 0u; break; // pop_or_invite
                        case 1: phasePriority = 1u; break; // starter_fill
                        case 2: phasePriority = 2u; break; // live_refill
                        case 3: phasePriority = 3u; break; // finish_fill
                        default: phasePriority = 9u; break;
                    }
                }

                return std::make_tuple(1u, phasePriority, desiredQueueType, requestTs ? requestTs : UINT32_MAX, botId);
            }

            return fallback;
        };

        availableBots.sort([&](uint32 a, uint32 b)
        {
            return rtgDispatchPriority(a) < rtgDispatchPriority(b);
        });
    }

    if (!availableBots.empty())
    {
        // Update bots
        for (auto bot : availableBots)
        {
            if (!GetPlayerBot(bot))
                continue;

            if (ProcessBot(bot))
            {
                updateBots--;
            }

            if (!updateBots)
                break;
        }

        if (loginBots)
        {
            loginBots += updateBots;
            loginBots = std::min(loginBots, maxNewBots);

            LOG_DEBUG("playerbots", "{} new bots prepared to login", loginBots);

            // Log in bots
            for (auto bot : availableBots)
            {
                if (GetPlayerBot(bot))
                    continue;

                if (ProcessBot(bot))
                {
                    loginBots--;
                }

                if (!loginBots)
                    break;
            }

            DelayLoginBotsTimer = 0;
        }
    }

    if (pmo)
        pmo->finish();

    if (sPlayerbotAIConfig.hasLog("player_location.csv"))
    {
        LogPlayerLocation();
    }
}

// void RandomPlayerbotMgr::ScaleBotActivity()
//{
//     float activityPercentage = getActivityPercentage();
//
//     // if (activityPercentage >= 100.0f || activityPercentage <= 0.0f) pid.reset(); //Stop integer buildup during
//     // max/min activity
//
//     //    % increase/decrease                   wanted diff                                         , avg diff
//     float activityPercentageMod = pid.calculate(
//         sRandomPlayerbotMgr.GetPlayers().empty() ? sPlayerbotAIConfig.diffEmpty :
//         sPlayerbotAIConfig.diffWithPlayer, sWorldUpdateTime.GetAverageUpdateTime());
//
//     activityPercentage = activityPercentageMod + 50;
//
//     // Cap the percentage between 0 and 100.
//     activityPercentage = std::max(0.0f, std::min(100.0f, activityPercentage));
//
//     setActivityPercentage(activityPercentage);
// }

// Assigns accounts as RNDbot accounts (type 1) based on MaxRandomBots and EnablePeriodicOnlineOffline and its ratio,
// and assigns accounts as AddClass accounts (type 2) based AddClassAccountPoolSize. Type 1 and 2 assignments are
// permenant, unless MaxRandomBots or AddClassAccountPoolSize are set to 0. If so, their associated accounts will
// be unassigned (type 0)
void RandomPlayerbotMgr::AssignAccountTypes()
{
    LOG_INFO("playerbots", "Assigning account types for random bot accounts...");

    // Clear existing filtered lists
    rndBotTypeAccounts.clear();
    addClassTypeAccounts.clear();

    // First, get ALL randombot accounts from the database
    std::vector<uint32> allRandomBotAccounts;
    QueryResult allAccounts = LoginDatabase.Query(
        "SELECT id FROM account WHERE username LIKE '{}%%' ORDER BY id",
        sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

    if (allAccounts)
    {
        do
        {
            Field* fields = allAccounts->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            allRandomBotAccounts.push_back(accountId);
        } while (allAccounts->NextRow());
    }

    LOG_INFO("playerbots", "Found {} total randombot accounts in database", allRandomBotAccounts.size());

// If the pool is empty or below desired count (e.g. after an admin wipe), auto-provision missing accounts.
// This prevents the server from getting stuck unable to log bots in.
uint32 desiredTotalAccounts = sPlayerbotAIConfig.randomBotAccountCount;

if (desiredTotalAccounts == 0)
{
    // Derive a sensible minimum if not configured
    int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
    int maxBots = static_cast<int>(RTG_GetStandaloneHelperCeiling());
    if (!sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        maxBots = static_cast<int>(maxBots * sPlayerbotAIConfig.periodicOnlineOfflineRatio);

    uint32 neededRnd = (maxBots > 0) ? static_cast<uint32>((maxBots + divisor - 1) / divisor) : 0;
    uint32 neededAdd = sPlayerbotAIConfig.addClassAccountPoolSize;
    desiredTotalAccounts = std::max<uint32>(neededRnd + neededAdd, 1);
}

if (allRandomBotAccounts.size() < desiredTotalAccounts)
{
    uint32 toCreate = desiredTotalAccounts - static_cast<uint32>(allRandomBotAccounts.size());
    LOG_INFO("playerbots", "Auto-creating {} randombot accounts to reach {} total...", toCreate, desiredTotalAccounts);

    // Create sequential usernames (prefix + 5 digits). Password defaults to username.
    for (uint32 i = 0; i < toCreate; ++i)
    {
        uint32 num = static_cast<uint32>(allRandomBotAccounts.size()) + 1 + i;
        std::ostringstream uname;
        uname << sPlayerbotAIConfig.randomBotAccountPrefix << std::setfill('0') << std::setw(5) << num;

        std::string username = uname.str();
        std::string password = username;

        AccountOpResult res = AccountMgr::CreateAccount(username, password, "");
        if (res == AccountOpResult::AOR_OK)
        {
            QueryResult r = LoginDatabase.Query("SELECT id FROM account WHERE username = '{}'", username);
            if (r)
            {
                Field* f = r->Fetch();
                allRandomBotAccounts.push_back(f[0].Get<uint32>());
            }
        }
        else
        {
            LOG_WARN("playerbots", "Failed to create randombot account {} (result={})", username, uint32(res));
        }
    }

    LOG_INFO("playerbots", "Randombot accounts after auto-create: {}", allRandomBotAccounts.size());
}


    // Check existing assignments
    QueryResult existingAssignments = PlayerbotsDatabase.Query("SELECT account_id, account_type FROM playerbots_account_type");
    std::map<uint32, uint8> currentAssignments;

    if (existingAssignments)
    {
        do
        {
            Field* fields = existingAssignments->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            uint8 accountType = fields[1].Get<uint8>();
            currentAssignments[accountId] = accountType;
        } while (existingAssignments->NextRow());
    }

    // Mark ALL randombot accounts as unassigned if not already assigned
    for (uint32 accountId : allRandomBotAccounts)
    {
        if (currentAssignments.find(accountId) == currentAssignments.end())
        {
            PlayerbotsDatabase.Execute("INSERT INTO playerbots_account_type (account_id, account_type) VALUES ({}, 0) ON DUPLICATE KEY UPDATE account_type = account_type", accountId);
            currentAssignments[accountId] = 0;
        }
    }

    // Calculate needed RNDbot accounts
    uint32 neededRndBotAccounts = 0;
    uint32 standaloneCeiling = RTG_GetStandaloneHelperCeiling();
    if (standaloneCeiling > 0)
    {
        int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
        int maxBots = static_cast<int>(standaloneCeiling);

        // Take periodic online-offline into account only for legacy random-bot sizing.
        if (!sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {
            maxBots *= sPlayerbotAIConfig.periodicOnlineOfflineRatio;
        }

        // Calculate base accounts needed for RNDbots, ensuring round up for maxBots not cleanly divisible by the divisor
        neededRndBotAccounts = (maxBots + divisor - 1) / divisor;
    }

    // Count existing assigned accounts
    uint32 existingRndBotAccounts = 0;
    uint32 existingAddClassAccounts = 0;

    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) existingRndBotAccounts++;
        else if (accountType == 2) existingAddClassAccounts++;
    }

    // Assign RNDbot accounts from lowest position if needed
    if (existingRndBotAccounts < neededRndBotAccounts)
    {
        uint32 toAssign = neededRndBotAccounts - existingRndBotAccounts;
        uint32 assigned = 0;

        for (uint32 i = 0; i < allRandomBotAccounts.size() && assigned < toAssign; i++)
        {
            uint32 accountId = allRandomBotAccounts[i];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabase.Execute("UPDATE playerbots_account_type SET account_type = 1, assignment_date = NOW() WHERE account_id = {}", accountId);
                currentAssignments[accountId] = 1;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill RNDbot requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Assign AddClass accounts from highest position if needed
    uint32 neededAddClassAccounts = sPlayerbotAIConfig.addClassAccountPoolSize;

    if (existingAddClassAccounts < neededAddClassAccounts)
    {
        uint32 toAssign = neededAddClassAccounts - existingAddClassAccounts;
        uint32 assigned = 0;

        for (size_t idx = allRandomBotAccounts.size(); idx-- > 0 && assigned < toAssign;)
        {
            uint32 accountId = allRandomBotAccounts[idx];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabase.Execute("UPDATE playerbots_account_type SET account_type = 2, assignment_date = NOW() WHERE account_id = {}", accountId);
                currentAssignments[accountId] = 2;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill AddClass requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Populate filtered account lists with ALL accounts of each type
    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) rndBotTypeAccounts.push_back(accountId);
        else if (accountType == 2) addClassTypeAccounts.push_back(accountId);
    }

    LOG_INFO("playerbots", "Account type assignment complete: {} RNDbot accounts, {} AddClass accounts, {} unassigned",
             rndBotTypeAccounts.size(), addClassTypeAccounts.size(),
             currentAssignments.size() - rndBotTypeAccounts.size() - addClassTypeAccounts.size());
}

bool RandomPlayerbotMgr::IsAccountType(uint32 accountId, uint8 accountType)
{
    QueryResult result = PlayerbotsDatabase.Query("SELECT 1 FROM playerbots_account_type WHERE account_id = {} AND account_type = {}", accountId, accountType);
    return result != nullptr;
}

// Logs-in bots in 4 phases. Phase 1 logs Alliance bots up to how much is expected according to the faction ratio,
// and Phase 2 logs-in the remainder Horde bots to reach the total maxAllowedBotCount. If maxAllowedBotCount is not
// reached after Phase 2, the function goes back to log-in Alliance bots and reach maxAllowedBotCount. This is done
// because not every account is guaranteed 5A/5H bots, so the true ratio might be skewed by few percentages. Finally,
// Phase 4 is reached if and only if the value of RandomBotAccountCount is lower than it should.
uint32 RandomPlayerbotMgr::AddRandomBots()
{
    uint32 maxAllowedBotCount = GetMaxAllowedBotCount();
    static time_t missingBotsTimer = 0;

    uint32 botsToAddThisInterval = 0;
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 rtgTarget = GetEventValue(0, "rtg_target");
        uint32 lfgNeed = GetEventValue(0, "rtg_lfg_need_total");
        uint32 bgNeed = GetEventValue(0, "rtg_bg_need_total");

        uint32 trackedManaged = 0;
        for (uint32 botId : currentBots)
        {
            if (!GetEventValue(botId, "add"))
                continue;

            std::string addData = GetEventData(botId, "add");
            if (!RTG::IsQueueManagedAddData(addData))
                continue;

            ++trackedManaged;
        }

        uint32 cappedLfgNeed = std::min<uint32>(lfgNeed, sPlayerbotAIConfig.rtgLfgMaxBots);
        uint32 cappedBgNeed = std::min<uint32>(bgNeed, sPlayerbotAIConfig.rtgBgMaxBots);
        uint32 unresolvedNeed = std::min<uint32>(cappedLfgNeed + cappedBgNeed, sPlayerbotAIConfig.rtgEventMaxBots);
        uint32 reconstructedTarget = std::min<uint32>(trackedManaged + unresolvedNeed, sPlayerbotAIConfig.rtgEventMaxBots);
        rtgTarget = reconstructedTarget;

        maxAllowedBotCount = rtgTarget;
        uint32 onlineHeadroom = maxAllowedBotCount > trackedManaged ? (maxAllowedBotCount - trackedManaged) : 0u;
        botsToAddThisInterval = std::min(unresolvedNeed, onlineHeadroom);

        if (RTG_QueueDebugEnabled())
        {
            LOG_INFO("playerbots", "[RTG][ACQUIRE][TARGET] legacyTarget={} rtgTarget={} trackedManaged={} unresolvedNeed={} lfgNeed={} bgNeed={} cappedLfgNeed={} cappedBgNeed={}",
                     GetMaxAllowedBotCount(), rtgTarget, trackedManaged, unresolvedNeed, lfgNeed, bgNeed, cappedLfgNeed, cappedBgNeed);
            LOG_INFO("playerbots", "[RTG][ACQUIRE][HEADROOM] need={} trackedManaged={} maxAllowed={} headroom={} addInterval={}",
                     unresolvedNeed, trackedManaged, maxAllowedBotCount, onlineHeadroom, botsToAddThisInterval);
        }
    }

    if ((sPlayerbotAIConfig.rtgEventDriven && botsToAddThisInterval > 0) || (!sPlayerbotAIConfig.rtgEventDriven && currentBots.size() < maxAllowedBotCount))
    {
        // Calculate how many bots to add
        if (sPlayerbotAIConfig.rtgEventDriven)
        {
            maxAllowedBotCount = botsToAddThisInterval;
        }
        else
        {
            maxAllowedBotCount -= currentBots.size();
            maxAllowedBotCount = std::min(sPlayerbotAIConfig.randomBotsPerInterval, maxAllowedBotCount);
        }

        // Single RNG instance for all shuffling
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

        // Only need to track the Alliance count, as it's in Phase 1
        uint32 totalRatio = sPlayerbotAIConfig.randomBotAllianceRatio + sPlayerbotAIConfig.randomBotHordeRatio;
        uint32 allowedAllianceCount = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) / totalRatio;

        uint32 remainder = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) % totalRatio;

        // Fix #1082: Randomly add one based on reminder
        if (remainder && urand(1, totalRatio) <= remainder)
        {
            allowedAllianceCount++;
        }

        // Determine which accounts to use.
        //
        // RTG event-driven helper acquisition must be able to draw from the full
        // RNDbot account pool. If periodic online/offline sharding is applied here,
        // multi-queue demand can look artificially account-starved even when the
        // realm has plenty of total bot accounts available. That was the root of
        // repeated "N more accounts needed" messages while two BGs and RDF were
        // active at the same time.
        std::vector<uint32> accountsToUse;
        if (!sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {
            // Legacy random-bot behavior: only use a rotating shard of accounts.
            uint32 accountsToUseCount = (rndBotTypeAccounts.size() + sPlayerbotAIConfig.periodicOnlineOfflineRatio - 1)
                                        / sPlayerbotAIConfig.periodicOnlineOfflineRatio;

            std::vector<uint32> shuffledAccounts = rndBotTypeAccounts;
            std::shuffle(shuffledAccounts.begin(), shuffledAccounts.end(), rng);

            for (uint32 i = 0; i < accountsToUseCount && i < shuffledAccounts.size(); i++)
                accountsToUse.push_back(shuffledAccounts[i]);
        }
        else
        {
            // RTG standalone queue-helper mode: use the full account pool.
            accountsToUse = rndBotTypeAccounts;
        }

        // Pre-map all characters from selected accounts
        struct CharacterInfo
        {
            uint32 guid;
            uint8 rClass;
            uint8 rRace;
            uint32 accountId;
        };
        std::vector<CharacterInfo> allCharacters;

        for (uint32 accountId : accountsToUse)
        {
            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARS_BY_ACCOUNT_ID);
            stmt->SetData(0, accountId);
            PreparedQueryResult result = CharacterDatabase.Query(stmt);
            if (!result)
                continue;

            do
            {
                Field* fields = result->Fetch();
                CharacterInfo info;
                info.guid = fields[0].Get<uint32>();
                info.rClass = fields[1].Get<uint8>();
                info.rRace = fields[2].Get<uint8>();
                info.accountId = accountId;
                allCharacters.push_back(info);
            } while (result->NextRow());
        }

        // Shuffle for class balance
        std::shuffle(allCharacters.begin(), allCharacters.end(), rng);

        // Separate characters by faction for phased login
        std::vector<CharacterInfo> allianceChars;
        std::vector<CharacterInfo> hordeChars;

        for (auto const& charInfo : allCharacters)
        {
            if (IsAlliance(charInfo.rRace))
                allianceChars.push_back(charInfo);

            else
                hordeChars.push_back(charInfo);
        }

        std::unordered_set<uint32> busyAccountIds;
        for (uint32 botId : currentBots)
        {
            uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(botId));
            if (accountId)
                busyAccountIds.insert(accountId);
        }

        for (auto const& [guid, bot] : playerBots)
        {
            if (!bot || !bot->IsInWorld())
                continue;

            uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
            if (accountId)
                busyAccountIds.insert(accountId);
        }

        if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
        {
            RTG::RtgQueueLedger& ledger = RTG::RtgQueueLedger::Instance();
            for (uint32 trackedBotId : ledger.GetTrackedBotIds())
            {
                RTG::RtgHelperLedgerEntry const* entry = ledger.Get(trackedBotId);
                if (!entry || !entry->accountId || entry->pendingRetire)
                    continue;

                if (entry->state == RTG::RtgHelperState::Retired)
                    continue;

                busyAccountIds.insert(entry->accountId);
            }
        }

        // Lambda to handle bot login logic
        auto tryLoginBot = [&](const CharacterInfo& charInfo, std::string const& addData = "") -> bool
        {
            if (busyAccountIds.find(charInfo.accountId) != busyAccountIds.end() ||
                GetEventValue(charInfo.guid, "add") ||
                GetEventValue(charInfo.guid, "logout") ||
                GetPlayerBot(charInfo.guid) ||
                std::find(currentBots.begin(), currentBots.end(), charInfo.guid) != currentBots.end() ||
                (sPlayerbotAIConfig.disableDeathKnightLogin && charInfo.rClass == CLASS_DEATH_KNIGHT))
            {
                return false;
            }

            uint32 add_time = sPlayerbotAIConfig.enablePeriodicOnlineOffline
                                ? urand(sPlayerbotAIConfig.minRandomBotInWorldTime,
                                        sPlayerbotAIConfig.maxRandomBotInWorldTime)
                                : sPlayerbotAIConfig.permanentlyInWorldTime;

            SetEventValue(charInfo.guid, "add", 1, add_time, addData);
            SetEventValue(charInfo.guid, "logout", 0, 0);
            if (RTG::HasPrefix(addData, "rtg_lfg:"))
            {
                SetEventValue(charInfo.guid, "rtg_lfg_pending", 1, 45, addData);
            }
            else if (RTG::HasPrefix(addData, "rtg_bg:"))
            {
                SetEventValue(charInfo.guid, "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
                SetEventValue(charInfo.guid, "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);
            }

            if (!addData.empty())
            {
                SetEventValue(charInfo.guid, "rtg_add_requested", NowSeconds(), 120, addData);
                RTG::RegisterPendingHelperLogin(charInfo.guid, charInfo.accountId, addData);
                RTG_RuntimeBreadcrumb(fmt::format("[RTG][ACQUIRE][REQUEST] helper={} account={} add='{}'", charInfo.guid, charInfo.accountId, addData));
            }

            busyAccountIds.insert(charInfo.accountId);

            // RTG event-driven helpers must enter the in-memory dispatch set immediately.
            // Relying on the later database-backed GetBots() sweep can delay or even starve
            // freshly acquired helpers when rtg_target/currentBots drift during multi-queue
            // finish-fill waves. That was producing repeated ACQUIRE->STALL patterns where
            // helpers were successfully reserved but not actually handed to ProcessBot in time.
            if (std::find(currentBots.begin(), currentBots.end(), charInfo.guid) == currentBots.end())
                currentBots.push_front(charInfo.guid);

            return true;
        };

        if (sPlayerbotAIConfig.rtgEventDriven)
        {
            struct RtgLfgBucket
            {
                uint32 owner = 0;
                uint32 team = 0;
                uint32 level = 0;
                uint32 realQueued = 0;
                uint32 realTank = 0;
                uint32 realHeal = 0;
                uint32 realDps = 0;
                uint32 assignedTank = 0;
                uint32 assignedHeal = 0;
                uint32 assignedDps = 0;
                uint32 queuedTank = 0;
                uint32 queuedHeal = 0;
                uint32 queuedDps = 0;
                uint32 need = 0;
                uint32 startTs = 0;
                bool activeDungeon = false;
            };

            struct RtgBgBucket
            {
                uint32 queueTypeId = 0;
                uint32 team = 0;
                uint32 level = 0;
                uint32 bracketId = 0;
                uint32 teamSize = 0;
                uint32 realQueued = 0;
                uint32 assignedExtra = 0;
                uint32 currentTeamCount = 0;
                uint32 need = 0;
                uint32 phase = 0;
            };

            std::map<std::tuple<uint32, uint32, uint32>, RtgLfgBucket> lfgBuckets;
            std::map<std::pair<uint32, uint32>, uint32> bgQueueTotals;
            std::map<std::tuple<uint32, uint32, uint32>, RtgBgBucket> bgBuckets;
            std::map<std::pair<uint32, uint32>, BattlegroundBracketId> bgBrackets;
            std::map<std::pair<uint32, uint32>, uint32> bgTeamSizes;

            for (Player* player : players)
            {
                if (!player || !player->IsInWorld() || IsRandomBot(player))
                    continue;

                Group* group = player->GetGroup();
				ObjectGuid queueGuid = player->GetGUID();

				if (group)
				{
					lfg::LfgState groupState = sLFGMgr->GetState(group->GetGUID());
					if (group->isLFGGroup() || groupState != lfg::LFG_STATE_NONE)
						queueGuid = group->GetGUID();
				}
                lfg::LfgState gState = sLFGMgr->GetState(queueGuid);
                Map* map = player->GetMap();
                bool queuedLfg = (gState != lfg::LFG_STATE_NONE && gState < lfg::LFG_STATE_DUNGEON);
                bool activeDungeon = (group && group->isLFGGroup()) ||
                                     gState == lfg::LFG_STATE_DUNGEON ||
                                     (map && (map->IsDungeon() || map->IsRaid()) && group && group->isLFGGroup());
                if (queuedLfg || activeDungeon)
                {
                    uint32 owner = queueGuid.GetCounter();
                    auto key = std::make_tuple(owner, static_cast<uint32>(player->GetTeamId()), static_cast<uint32>(player->GetLevel()));
                    RtgLfgBucket& bucket = lfgBuckets[key];
                    bucket.owner = owner;
                    bucket.team = player->GetTeamId();
                    bucket.level = player->GetLevel();
                    bucket.startTs = activeDungeon ? (static_cast<uint32>(time(nullptr)) - sPlayerbotAIConfig.rtgQueueGraceSeconds) : GetEventValue(owner, "rtg_lfg_start");
                    if (activeDungeon)
                        bucket.activeDungeon = true;
                    if (queuedLfg)
                        ++bucket.realQueued;

                    uint32 role = RTG_NormalizeQueuedRoleMask(sLFGMgr->GetRoles(player->GetGUID()));
                    if (!role)
                        role = RTG_DefaultRoleForClass(player->getClass());

                    if (role == lfg::PLAYER_ROLE_TANK)
                        ++bucket.realTank;
                    else if (role == lfg::PLAYER_ROLE_HEALER)
                        ++bucket.realHeal;
                    else
                        ++bucket.realDps;
                }

                if (!player->InBattlegroundQueue() && !player->InBattleground())
                    continue;

                for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
                {
                    BattlegroundQueueTypeId queueTypeId = player->GetBattlegroundQueueTypeId(queueType);
                    if (queueTypeId <= BATTLEGROUND_QUEUE_NONE || queueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
                        continue;
                    if (BattlegroundMgr::BGArenaType(queueTypeId))
                        continue;

                    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
                    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
                        continue;

                    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
                    if (!bgTemplate)
                        continue;

                    PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
                    if (!pvpDiff)
                        continue;

                    auto bgKey = std::make_pair(static_cast<uint32>(queueTypeId), static_cast<uint32>(player->GetLevel()));
                    ++bgQueueTotals[bgKey];
                    bgBrackets[bgKey] = pvpDiff->GetBracketId();
                    bgTeamSizes[bgKey] = bgTemplate->GetMaxPlayersPerTeam();
                }
            }

            for (uint32 botId : currentBots)
            {
                if (!GetEventValue(botId, "add"))
                    continue;

                std::string addData = GetEventData(botId, "add");
                Player* managedBot = GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(botId));

                uint32 dataTeam = 0;
                uint32 dataLevel = 0;
                uint32 desiredRole = 0;
                uint32 desiredQueueType = 0;

                uint32 ownerId = 0;
                if (RTG::ParseLfgAddData(addData, dataTeam, dataLevel, &desiredRole, &ownerId))
                {
                    auto it = lfgBuckets.find(std::make_tuple(ownerId, dataTeam, dataLevel));
                    if (it == lfgBuckets.end())
                        continue;

                    uint32 roleToCount = desiredRole ? desiredRole : lfg::PLAYER_ROLE_DAMAGE;
                    bool countsInLfgState = false;
                    if (managedBot && managedBot->IsInWorld())
                    {
                        lfg::LfgState botState = sLFGMgr->GetState(managedBot->GetGUID());
                        countsInLfgState = (botState != lfg::LFG_STATE_NONE && botState < lfg::LFG_STATE_DUNGEON);
                    }

                    if (countsInLfgState)
                    {
                        if (roleToCount == lfg::PLAYER_ROLE_TANK)
                            ++it->second.queuedTank;
                        else if (roleToCount == lfg::PLAYER_ROLE_HEALER)
                            ++it->second.queuedHeal;
                        else
                            ++it->second.queuedDps;
                    }
                    else
                    {
                        if (roleToCount == lfg::PLAYER_ROLE_TANK)
                            ++it->second.assignedTank;
                        else if (roleToCount == lfg::PLAYER_ROLE_HEALER)
                            ++it->second.assignedHeal;
                        else
                            ++it->second.assignedDps;
                    }
                }
                else if (RTG::ParseBgAddData(addData, dataTeam, dataLevel, desiredQueueType))
                {
                    auto key = std::make_tuple(desiredQueueType, dataTeam, dataLevel);
                    auto it = bgBuckets.find(key);
                    if (it == bgBuckets.end())
                    {
                        BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;
                        uint32 minLevel = 0;
                        uint32 maxLevel = 0;
                        if (!RTG_GetBgQueueContext(BattlegroundQueueTypeId(desiredQueueType), dataLevel, bracketId, minLevel, maxLevel))
                            continue;

                        RtgBgBucket bucket;
                        bucket.queueTypeId = desiredQueueType;
                        bucket.team = dataTeam;
                        bucket.level = dataLevel;
                        bucket.bracketId = bracketId;
                        bucket.teamSize = bgTeamSizes[std::make_pair(desiredQueueType, dataLevel)];
                        bucket.realQueued = bgQueueTotals[std::make_pair(desiredQueueType, dataLevel)];
                        bucket.phase = GetEventValue(0, RTG_MakeBgPhaseKey(desiredQueueType, uint32(bracketId)));

                        BattlegroundInfo& bgInfo = BattlegroundData[desiredQueueType][bracketId];
                        bucket.currentTeamCount = (dataTeam == TEAM_ALLIANCE)
                            ? (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount)
                            : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount);
                        bucket.need = GetEventValue(0, RTG_MakeBgTeamNeedKey(desiredQueueType, uint32(bracketId), dataTeam));

                        it = bgBuckets.emplace(key, bucket).first;
                    }

                    bool countsInBgState = false;
                    if (managedBot && managedBot->IsInWorld())
                    {
                        countsInBgState = managedBot->InBattlegroundQueueForBattlegroundQueueType(BattlegroundQueueTypeId(desiredQueueType)) ||
                                          (managedBot->InBattleground() && managedBot->GetBattlegroundTypeId() == BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(desiredQueueType)));
                    }

                    if (!countsInBgState)
                        ++it->second.assignedExtra;
                }
            }

            uint32 rtgNow = static_cast<uint32>(time(nullptr));
            if (RTG_QueueDebugEnabled())
            {
                LOG_INFO("playerbots", "[RTG][ACQUIRE][PLAN] chars={} lfgBuckets={} bgBuckets={} currentBots={} targetBots={}",
                         static_cast<uint32>(allCharacters.size()),
                         static_cast<uint32>(lfgBuckets.size()),
                         static_cast<uint32>(bgBuckets.size()),
                         static_cast<uint32>(currentBots.size()),
                         maxAllowedBotCount);
            }
            for (auto& kv : lfgBuckets)
            {
                RtgLfgBucket& bucket = kv.second;
                if (bucket.startTs && rtgNow < bucket.startTs + sPlayerbotAIConfig.rtgQueueGraceSeconds)
                {
                    bucket.need = 0;
                    continue;
                }

                uint32 needTank = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_TANK);
                needTank = needTank > (bucket.realTank + bucket.queuedTank + bucket.assignedTank) ? (needTank - (bucket.realTank + bucket.queuedTank + bucket.assignedTank)) : 0u;
                uint32 needHeal = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_HEALER);
                needHeal = needHeal > (bucket.realHeal + bucket.queuedHeal + bucket.assignedHeal) ? (needHeal - (bucket.realHeal + bucket.queuedHeal + bucket.assignedHeal)) : 0u;
                uint32 needDps = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_DAMAGE);
                needDps = needDps > (bucket.realDps + bucket.queuedDps + bucket.assignedDps) ? (needDps - (bucket.realDps + bucket.queuedDps + bucket.assignedDps)) : 0u;
                bucket.need = needTank + needHeal + needDps;
            }

            for (auto const& queueTypePair : BattlegroundData)
            {
                uint32 queueTypeId = queueTypePair.first;
                for (auto const& bracketPair : queueTypePair.second)
                {
                    BattlegroundBracketId bracketId = static_cast<BattlegroundBracketId>(bracketPair.first);
                    BattlegroundInfo const& bgInfo = bracketPair.second;
                    if (!bgInfo.minLevel)
                        continue;

                    uint32 level = bgInfo.maxLevel ? bgInfo.maxLevel : bgInfo.minLevel;
                    uint32 phase = GetEventValue(0, RTG_MakeBgPhaseKey(queueTypeId, uint32(bracketId)));

                    for (uint32 team : {static_cast<uint32>(TEAM_ALLIANCE), static_cast<uint32>(TEAM_HORDE)})
                    {
                        uint32 plannerNeed = GetEventValue(0, RTG_MakeBgTeamNeedKey(queueTypeId, uint32(bracketId), team));
                        auto key = std::make_tuple(queueTypeId, team, level);
                        auto it = bgBuckets.find(key);
                        if (it == bgBuckets.end())
                        {
                            RtgBgBucket bucket;
                            bucket.queueTypeId = queueTypeId;
                            bucket.team = team;
                            bucket.level = level;
                            bucket.bracketId = bracketId;
                            bucket.teamSize = (team == TEAM_ALLIANCE)
                                ? (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount + plannerNeed)
                                : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount + plannerNeed);
                            bucket.realQueued = (team == TEAM_ALLIANCE) ? bgInfo.bgQueueAlliancePlayerCount : bgInfo.bgQueueHordePlayerCount;
                            bucket.currentTeamCount = (team == TEAM_ALLIANCE)
                                ? (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount)
                                : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount);
                            bucket.phase = phase;
                            bucket.need = plannerNeed;
                            it = bgBuckets.emplace(key, bucket).first;
                        }
                        else
                        {
                            it->second.bracketId = bracketId;
                            it->second.level = level;
                            it->second.phase = phase;
                            it->second.currentTeamCount = (team == TEAM_ALLIANCE)
                                ? (bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount)
                                : (bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount);
                            it->second.need = plannerNeed;
                        }
                    }
                }
            }

            std::vector<RtgLfgBucket> orderedLfgBuckets;
            std::vector<RtgBgBucket> orderedBgBuckets;
            uint32 totalLfgNeed = 0;
            uint32 totalBgNeed = 0;

            for (auto const& kv : lfgBuckets)
            {
                if (kv.second.need)
                {
                    orderedLfgBuckets.push_back(kv.second);
                    totalLfgNeed += kv.second.need;
                }
            }

            for (auto const& kv : bgBuckets)
            {
                if (kv.second.need)
                {
                    orderedBgBuckets.push_back(kv.second);
                    totalBgNeed += kv.second.need;
                }
            }

            std::sort(orderedLfgBuckets.begin(), orderedLfgBuckets.end(), [](RtgLfgBucket const& a, RtgLfgBucket const& b)
            {
                if (a.startTs != b.startTs)
                    return (a.startTs ? a.startTs : UINT32_MAX) < (b.startTs ? b.startTs : UINT32_MAX);
                if (a.need != b.need)
                    return a.need > b.need;
                if (a.realQueued != b.realQueued)
                    return a.realQueued > b.realQueued;
                if (a.team != b.team)
                    return a.team < b.team;
                return a.level > b.level;
            });

            auto rtgBgPhasePriority = [](uint32 phase) -> uint32
            {
                switch (phase)
                {
                    case 0: return 0; // pop_or_invite
                    case 1: return 1; // starter_fill
                    case 2: return 2; // live_refill
                    case 3: return 3; // finish_fill
                    default: return 4;
                }
            };

            std::sort(orderedBgBuckets.begin(), orderedBgBuckets.end(), [&](RtgBgBucket const& a, RtgBgBucket const& b)
            {
                uint32 aPriority = rtgBgPhasePriority(a.phase);
                uint32 bPriority = rtgBgPhasePriority(b.phase);
                if (aPriority != bPriority)
                    return aPriority < bPriority;
                if (a.need != b.need)
                    return a.need > b.need;
                if (a.realQueued != b.realQueued)
                    return a.realQueued > b.realQueued;
                if (a.queueTypeId != b.queueTypeId)
                    return a.queueTypeId < b.queueTypeId;
                if (a.team != b.team)
                    return a.team < b.team;
                return a.level > b.level;
            });

            bool bgHasStartupNeed = false;
            bool bgHasStarterNeed = false;
            for (RtgBgBucket const& bucket : orderedBgBuckets)
            {
                if (!bucket.need)
                    continue;
                if (bucket.phase == 0)
                    bgHasStartupNeed = true;
                else if (bucket.phase == 1)
                    bgHasStarterNeed = true;
            }

            bool suppressBgFinishFill = bgHasStartupNeed || bgHasStarterNeed;

            if (RTG_QueueDebugEnabled())
            {
                LOG_INFO("playerbots", "[RTG][ACQUIRE][BG_ORDER] startupNeed={} starterNeed={} suppressFinishFill={}",
                         bgHasStartupNeed ? 1u : 0u,
                         bgHasStarterNeed ? 1u : 0u,
                         suppressBgFinishFill ? 1u : 0u);
            }

            uint32 remainingCapacity = maxAllowedBotCount;
            uint32 totalRequested = totalLfgNeed + totalBgNeed;
            uint32 lfgCapacity = 0;
            uint32 bgCapacity = 0;
            if (totalRequested)
            {
                lfgCapacity = (remainingCapacity * totalLfgNeed + totalRequested - 1) / totalRequested;
                if (lfgCapacity > remainingCapacity)
                    lfgCapacity = remainingCapacity;
                bgCapacity = remainingCapacity - lfgCapacity;
                if (!totalLfgNeed)
                    bgCapacity = remainingCapacity;
                if (!totalBgNeed)
                    lfgCapacity = remainingCapacity;

                // Groundwork for the RTG role/context framework:
                // when both RDF and BG need support, do not allow one queue type to starve the other.
                if (totalLfgNeed && totalBgNeed)
                {
                    if (remainingCapacity == 1)
                    {
                        bool bgTurn = GetEventValue(0, "rtg_queue_turn_bg") != 0;
                        lfgCapacity = bgTurn ? 0u : 1u;
                        bgCapacity = bgTurn ? 1u : 0u;
                        SetEventValue(0, "rtg_queue_turn_bg", bgTurn ? 0u : 1u, 30);
                    }
                    else if (remainingCapacity >= 2)
                    {
                        if (!lfgCapacity && bgCapacity)
                        {
                            lfgCapacity = 1;
                            --bgCapacity;
                        }
                        if (!bgCapacity && lfgCapacity)
                        {
                            bgCapacity = 1;
                            --lfgCapacity;
                        }
                    }
                }
            }

            uint32 rtgLfgLogged = 0;
            uint32 rtgBgLogged = 0;

            auto tryFillLfgRole = [&](RtgLfgBucket& bucket, uint32 desiredRole, uint32& capacity) -> bool
            {
                if (!capacity)
                    return false;

                std::string addData = RTG::MakeLfgAddData(bucket.team, bucket.level, desiredRole, bucket.owner);
                for (auto const& charInfo : allCharacters)
                {
                    if (!capacity)
                        return false;

                    uint32 charTeam = IsAlliance(charInfo.rRace) ? TEAM_ALLIANCE : TEAM_HORDE;
                    if (charTeam != bucket.team)
                        continue;
                    if (RTG_GetOfflineSpecRole(charInfo.guid, charInfo.rClass) != desiredRole)
                        continue;
                    if (!tryLoginBot(charInfo, addData))
                        continue;

                    LOG_INFO("playerbots", "[RTG][LFG][ACQUIRE] Logged helper bot {} for owner {} as desired role {} (class {})", charInfo.guid, bucket.owner, desiredRole, charInfo.rClass);

                    ++rtgLfgLogged;
                    --capacity;
                    --remainingCapacity;
                    if (desiredRole == lfg::PLAYER_ROLE_TANK)
                        ++bucket.assignedTank;
                    else if (desiredRole == lfg::PLAYER_ROLE_HEALER)
                        ++bucket.assignedHeal;
                    else
                        ++bucket.assignedDps;
                    return true;
                }
                return false;
            };

            auto tryFillLfgBucketOnce = [&](RtgLfgBucket& bucket, uint32& capacity) -> bool
            {
                if (!capacity || !remainingCapacity)
                    return false;
                if (bucket.startTs && static_cast<uint32>(time(nullptr)) < bucket.startTs + sPlayerbotAIConfig.rtgQueueGraceSeconds)
                    return false;

                uint32 needTank = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_TANK);
                needTank = needTank > (bucket.realTank + bucket.queuedTank + bucket.assignedTank) ? (needTank - (bucket.realTank + bucket.queuedTank + bucket.assignedTank)) : 0u;
                uint32 needHeal = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_HEALER);
                needHeal = needHeal > (bucket.realHeal + bucket.queuedHeal + bucket.assignedHeal) ? (needHeal - (bucket.realHeal + bucket.queuedHeal + bucket.assignedHeal)) : 0u;
                uint32 needDps = RTG_TargetLfgRoleCount(lfg::PLAYER_ROLE_DAMAGE);
                needDps = needDps > (bucket.realDps + bucket.queuedDps + bucket.assignedDps) ? (needDps - (bucket.realDps + bucket.queuedDps + bucket.assignedDps)) : 0u;
                if (!needTank && !needHeal && !needDps)
                    return false;

                bool added = false;
                if (needTank)
                    added = tryFillLfgRole(bucket, lfg::PLAYER_ROLE_TANK, capacity);
                if (!added && needHeal)
                    added = tryFillLfgRole(bucket, lfg::PLAYER_ROLE_HEALER, capacity);
                if (!added && needDps)
                    added = tryFillLfgRole(bucket, lfg::PLAYER_ROLE_DAMAGE, capacity);
                return added;
            };

            auto fillReservedLfgLane = [&](uint32& capacity, bool /*bgDemandActive*/)
            {
                // RTG doctrine: RDF/LFG startup must retain its own reserved lane.
                // Battleground demand may compete for shared surplus later, but it must
                // not suppress fresh RDF assembly inside the LFG-reserved slice.
                for (RtgLfgBucket& bucket : orderedLfgBuckets)
                {
                    while (capacity && remainingCapacity && tryFillLfgBucketOnce(bucket, capacity))
                    {
                    }
                }
            };

            auto tryFillBgBucket = [&](RtgBgBucket& bucket, uint32& capacity) -> bool
            {
                if (!capacity)
                    return false;

                std::string addData = RTG::MakeBgAddData(bucket.team, bucket.level, bucket.queueTypeId);
                for (auto const& charInfo : allCharacters)
                {
                    if (!capacity)
                        return false;

                    uint32 charTeam = IsAlliance(charInfo.rRace) ? TEAM_ALLIANCE : TEAM_HORDE;
                    if (charTeam != bucket.team)
                        continue;
                    if (!tryLoginBot(charInfo, addData))
                        continue;

                    LOG_INFO("playerbots", "[RTG][BG][ACQUIRE] Logged helper bot {} for queue {} team {} level {}", charInfo.guid, bucket.queueTypeId, bucket.team, bucket.level);
                    RTG_RuntimeBreadcrumb(fmt::format("[RTG][ACQUIRE] helper={} queue={} team={} level={}", charInfo.guid, bucket.queueTypeId, bucket.team, bucket.level));
                    ++rtgBgLogged;
                    --capacity;
                    --remainingCapacity;
                    ++bucket.assignedExtra;
                    return true;
                }
                return false;
            };

            auto tryFillBgBucketOnce = [&](RtgBgBucket& bucket, uint32& capacity) -> bool
            {
                if (!capacity || !remainingCapacity)
                    return false;
                if (bucket.assignedExtra >= bucket.need)
                    return false;

                uint32 pendingGlobal = RTG_CountPendingHelpers();
                if (pendingGlobal >= RTG_GetPendingHelperLoginGlobalCap())
                    return false;

                uint32 pendingLane = RTG_CountPendingHelpers(bucket.queueTypeId, uint32(bucket.bracketId), bucket.team);
                if (pendingLane >= RTG_GetPendingHelperLoginLaneCap(bucket.phase))
                    return false;

                return tryFillBgBucket(bucket, capacity);
            };

            auto fillReservedBgLane = [&](uint32& capacity)
            {
                bool added = true;
                while (capacity && remainingCapacity && added)
                {
                    added = false;
                    for (RtgBgBucket& bucket : orderedBgBuckets)
                    {
                        if (!capacity || !remainingCapacity)
                            break;
                        if (suppressBgFinishFill && bucket.phase == 3)
                            continue;
                        if (tryFillBgBucketOnce(bucket, capacity))
                            added = true;
                    }
                }
            };

            uint32 reservedLfgCapacity = lfgCapacity;
            uint32 reservedBgCapacity = bgCapacity;
            uint32 laneDebugInitialLfg = reservedLfgCapacity;
            uint32 laneDebugInitialBg = reservedBgCapacity;
            bool bgDemandActive = totalBgNeed > 0;

            fillReservedLfgLane(reservedLfgCapacity, bgDemandActive);
            fillReservedBgLane(reservedBgCapacity);

            uint32 sharedSurplus = reservedLfgCapacity + reservedBgCapacity;
            lfgCapacity = 0;
            bgCapacity = 0;

            if (sharedSurplus && remainingCapacity)
            {
                bool bgTurn = GetEventValue(0, "rtg_queue_turn_bg") != 0;
                bool added = true;

                while (sharedSurplus && remainingCapacity && added)
                {
                    added = false;

                    auto tryLane = [&](bool preferBg) -> bool
                    {
                        if (preferBg)
                        {
                            for (RtgBgBucket& bucket : orderedBgBuckets)
                            {
                                if (suppressBgFinishFill && bucket.phase == 3)
                                    continue;
                                if (tryFillBgBucketOnce(bucket, sharedSurplus))
                                    return true;
                            }
                            for (RtgLfgBucket& bucket : orderedLfgBuckets)
                            {
                                if (tryFillLfgBucketOnce(bucket, sharedSurplus))
                                    return true;
                            }
                        }
                        else
                        {
                            for (RtgLfgBucket& bucket : orderedLfgBuckets)
                            {
                                if (tryFillLfgBucketOnce(bucket, sharedSurplus))
                                    return true;
                            }
                            for (RtgBgBucket& bucket : orderedBgBuckets)
                            {
                                if (suppressBgFinishFill && bucket.phase == 3)
                                    continue;
                                if (tryFillBgBucketOnce(bucket, sharedSurplus))
                                    return true;
                            }
                        }
                        return false;
                    };

                    if (tryLane(bgTurn))
                    {
                        added = true;
                        bgTurn = !bgTurn;
                    }
                }

                SetEventValue(0, "rtg_queue_turn_bg", bgTurn ? 1u : 0u, 30);
            }

            if (RTG_QueueDebugEnabled())
            {
                LOG_INFO("playerbots", "[RTG][ACQUIRE][LANES] lfgReserved={} bgReserved={} lfgUnused={} bgUnused={} sharedSurplus={} remainingCapacity={} bgDemandActive={}",
                         laneDebugInitialLfg,
                         laneDebugInitialBg,
                         reservedLfgCapacity,
                         reservedBgCapacity,
                         sharedSurplus,
                         remainingCapacity,
                         bgDemandActive ? 1u : 0u);
            }

            if (RTG_QueueDebugEnabled())
                LOG_INFO("playerbots", "[RTG][ACQUIRE][RESULT] loggedLfg={} loggedBg={} remainingCapacity={} totalLfgNeed={} totalBgNeed={}", rtgLfgLogged, rtgBgLogged, remainingCapacity, totalLfgNeed, totalBgNeed);

            if (remainingCapacity)
            {
                if (missingBotsTimer == 0)
                    missingBotsTimer = time(nullptr);

                if (RTG_QueueDebugEnabled())
                {
                    LOG_INFO("playerbots", "[RTG][ACQUIRE][MISS] no more offline candidates available for current RTG demand; remainingCapacity={} allCharacters={} currentBots={}",
                             remainingCapacity, static_cast<uint32>(allCharacters.size()), static_cast<uint32>(currentBots.size()));
                }

                if (time(nullptr) - missingBotsTimer >= 10 && (totalLfgNeed || totalBgNeed))
                {
                    int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
                    uint32 moreAccountsNeeded = (remainingCapacity + divisor - 1) / divisor;
                    LOG_ERROR("playerbots",
                              "Can't log-in all the requested bots. Try increasing RandomBotAccountCount in your conf file.\n"
                              "{} more accounts needed.", moreAccountsNeeded);
                    missingBotsTimer = 0;
                }
            }
            else
            {
                missingBotsTimer = 0;
            }

            return currentBots.size();
        }

        // PHASE 1: Log-in Alliance bots up to allowedAllianceCount
        for (auto const& charInfo : allianceChars)
        {
            if (!allowedAllianceCount)
                break;

            if (tryLoginBot(charInfo))
            {
                maxAllowedBotCount--;
                allowedAllianceCount--;
            }
        }

        // PHASE 2: Log-in Horde bots up to maxAllowedBotCount
        for (auto const& charInfo : hordeChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 3: If maxAllowedBotCount wasn't reached, log-in more Alliance bots
        for (auto const& charInfo : allianceChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 4: An error is given if maxAllowedBotCount is still not reached
        if (maxAllowedBotCount)
        {
            if (missingBotsTimer == 0)
                missingBotsTimer = time(nullptr);

            if (time(nullptr) - missingBotsTimer >= 10)
            {
                int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
                uint32 moreAccountsNeeded = (maxAllowedBotCount + divisor - 1) / divisor;
                LOG_ERROR("playerbots",
                          "Can't log-in all the requested bots. Try increasing RandomBotAccountCount in your conf file.\n"
                          "{} more accounts needed.", moreAccountsNeeded);
                missingBotsTimer = 0;    // Reset timer so error is not spammed every tick
            }
        }
        else
        {
            missingBotsTimer = 0;       // Reset timer if logins for this interval were successful
        }
    }
    else
    {
        missingBotsTimer = 0;           // Reset timer if there's enough bots
    }

    return currentBots.size();
}

void RandomPlayerbotMgr::LoadBattleMastersCache()
{
    BattleMastersCache.clear();

    LOG_INFO("playerbots", "Loading Battlemasters Cache...");

    QueryResult result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        return;
    }

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].Get<uint32>();
        uint32 bgTypeId = fields[1].Get<uint32>();

        CreatureTemplate const* bmaster = sObjectMgr->GetCreatureTemplate(entry);
        if (!bmaster)
            continue;

        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->faction);
        uint32 bmFactionId = bmFaction->faction;
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
        uint32 bmParentTeam = bmParentFaction->team;
        TeamId bmTeam = TEAM_NEUTRAL;
        if (bmParentTeam == 891)
            bmTeam = TEAM_ALLIANCE;

        if (bmFactionId == 189)
            bmTeam = TEAM_ALLIANCE;

        if (bmParentTeam == 892)
            bmTeam = TEAM_HORDE;

        if (bmFactionId == 66)
            bmTeam = TEAM_HORDE;

        BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].insert(
            BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].end(), entry);
        LOG_DEBUG("playerbots", "Cached Battlemaster #{} for BG Type {} ({})", entry, bgTypeId,
                  bmTeam == TEAM_ALLIANCE ? "Alliance"
                  : bmTeam == TEAM_HORDE  ? "Horde"
                                          : "Neutral");

    } while (result->NextRow());

    LOG_INFO("playerbots", ">> Loaded {} battlemaster entries", count);
}

std::vector<uint32> parseBrackets(const std::string& str)
{
    std::vector<uint32> brackets;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        brackets.push_back(static_cast<uint32>(std::stoi(item)));
    }

    return brackets;
}

void RandomPlayerbotMgr::CheckBgQueue()
{
    RTG_RunQueueOwnershipAudit();

    if (!BgCheckTimer)
    {
        BgCheckTimer = time(nullptr);
        return;  // Exit immediately after initializing the timer
    }

    if (time(nullptr) < BgCheckTimer)
    {
        return;  // No need to proceed if the current time is less than the timer
    }

    // Update the timer to the current time
    BgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking BG Queue...");
    if (RTG_QueueDebugEnabled())
        LOG_INFO("playerbots", "[RTGDBG][BG] check begin trackedPlayers={} trackedBots={}", static_cast<uint32>(players.size()), static_cast<uint32>(playerBots.size()));

    bool anyRealQueued = false;

    if (sPlayerbotAIConfig.rtgEventDriven && GetOnlineRealPlayerCount() == 0)
    {
        SetEventValue(0, "rtg_bg_any_real_queued", 0, 0);
        SetEventValue(0, "rtg_bg_any_real_demand", 0, 0);
        SetEventValue(0, "rtg_bg_need_total", 0, 0);
        SetEventValue(0, "rtg_bg_start", 0, 0);

        if (RTG_QueueDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][BG] skip check: no real players online");
        return;
    }

    // Initialize Battleground Data (do not clear here)

    for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
    {
        for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            BattlegroundData[queueType][bracket] = BattlegroundInfo();
        }
    }

    // Process real players and populate Battleground Data with player/queue count
    // Opens a queue for bots to join
    std::unordered_set<uint64> rtgBgParticipantSeen;

    auto rtgRecordBgParticipant = [&](ObjectGuid guid, TeamId teamId, bool isBot, BattlegroundQueueTypeId queueTypeId,
                                      BattlegroundBracketId bracketId, uint32 minLevel, uint32 maxLevel,
                                      bool queueState, bool activeState, uint32 instanceId = 0)
    {
        BattlegroundInfo& bgInfo = BattlegroundData[queueTypeId][bracketId];
        bgInfo.minLevel = minLevel;
        bgInfo.maxLevel = maxLevel;

        RTG_RecordBgTeamCounts(bgInfo, teamId, isBot, queueState, activeState);

        if (activeState && instanceId)
        {
            std::vector<uint32>& instanceIds = bgInfo.bgInstances;
            if (std::find(instanceIds.begin(), instanceIds.end(), instanceId) == instanceIds.end())
                instanceIds.push_back(instanceId);
            bgInfo.bgInstanceCount = instanceIds.size();
        }

        uint64 seenKey = RTG_MakeBgParticipantKey(uint32(queueTypeId), bracketId, guid, isBot);
        if (!rtgBgParticipantSeen.insert(seenKey).second)
            return;

        if (isBot && sPlayerbotAIConfig.rtgQueueOwnershipEnable)
        {
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
                RTG::SyncBgHelperState(bot, uint32(queueTypeId), bracketId, &bgInfo);
        }

        if (isBot)
        {
            if (teamId == TEAM_ALLIANCE)
                ++bgInfo.bgAllianceBotCount;
            else
                ++bgInfo.bgHordeBotCount;
        }
        else
        {
            if (teamId == TEAM_ALLIANCE)
                ++bgInfo.bgAlliancePlayerCount;
            else
                ++bgInfo.bgHordePlayerCount;
        }
    };

    for (Player* player : players)
    {
        if (!player || IsRandomBot(player))
            continue;

        bool inQueue = player->InBattlegroundQueue();
        bool inBg = player->InBattleground();
        if (!inQueue && !inBg)
            continue;

        anyRealQueued = true;

        Battleground* bg = player->GetBattleground();
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = player->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = player->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            BattlegroundBracketId bracketId;
            uint32 minLevel = 0;
            uint32 maxLevel = 0;
            if (!RTG_GetBgQueueContext(queueTypeId, player->GetLevel(), bracketId, minLevel, maxLevel))
                continue;

            bool isArena = BattlegroundMgr::BGArenaType(queueTypeId) != 0;
            if (isArena)
            {
                bool isRated = false;
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
                    isRated = ginfo.IsRated;

                if (bgQueue.IsPlayerInvitedToRatedArena(player->GetGUID()) ||
                    (player->InArena() && player->GetBattleground() && player->GetBattleground()->isRated()))
                    isRated = true;

                BattlegroundInfo& arenaInfo = BattlegroundData[queueTypeId][bracketId];
                arenaInfo.minLevel = minLevel;
                arenaInfo.maxLevel = maxLevel;

                if (isRated)
                    ++arenaInfo.ratedArenaPlayerCount;
                else
                    ++arenaInfo.skirmishArenaPlayerCount;

                if (!player->IsInvitedForBattlegroundInstance() && !player->InBattleground())
                {
                    if (isRated)
                        arenaInfo.activeRatedArenaQueue = 1;
                    else
                        arenaInfo.activeSkirmishArenaQueue = 1;
                }

                if (bg)
                {
                    std::vector<uint32>* instanceIds = nullptr;
                    if (player->InArena() && bg->isRated())
                        instanceIds = &arenaInfo.ratedArenaInstances;
                    else if (player->InArena())
                        instanceIds = &arenaInfo.skirmishArenaInstances;

                    if (instanceIds && std::find(instanceIds->begin(), instanceIds->end(), bg->GetInstanceID()) == instanceIds->end())
                        instanceIds->push_back(bg->GetInstanceID());

                    if (player->InArena() && bg->isRated())
                        arenaInfo.ratedArenaInstanceCount = arenaInfo.ratedArenaInstances.size();
                    else if (player->InArena())
                        arenaInfo.skirmishArenaInstanceCount = arenaInfo.skirmishArenaInstances.size();
                }

                continue;
            }

            bool queueState = player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
            bool activeState = bg && bg->GetBgTypeID() == BattlegroundMgr::BGTemplateId(queueTypeId);
            rtgRecordBgParticipant(player->GetGUID(), teamId, false, queueTypeId, bracketId, minLevel, maxLevel, queueState, activeState,
                                   activeState ? bg->GetInstanceID() : 0);

            if (queueState && !player->IsInvitedForBattlegroundInstance() && !player->InBattleground())
                BattlegroundData[queueTypeId][bracketId].activeBgQueue = 1;
        }
    }

    // Process player bots
    for (auto& [guid, bot] : playerBots)
    {
        if (!bot || !bot->IsInWorld() || !IsRandomBot(bot))
            continue;

        bool inQueue = bot->InBattlegroundQueue();
        bool inBg = bot->InBattleground();
        if (!inQueue && !inBg)
            continue;

        Battleground* bg = bot->GetBattleground();
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = bot->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            BattlegroundBracketId bracketId;
            uint32 minLevel = 0;
            uint32 maxLevel = 0;
            if (!RTG_GetBgQueueContext(queueTypeId, bot->GetLevel(), bracketId, minLevel, maxLevel))
                continue;

            if (uint8 arenaType = BattlegroundMgr::BGArenaType(queueTypeId))
            {
                bool isRated = false;
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(guid, &ginfo))
                    isRated = ginfo.IsRated;

                if (bgQueue.IsPlayerInvitedToRatedArena(guid) || (bot->InArena() && bot->GetBattleground() && bot->GetBattleground()->isRated()))
                    isRated = true;

                BattlegroundInfo& arenaInfo = BattlegroundData[queueTypeId][bracketId];
                arenaInfo.minLevel = minLevel;
                arenaInfo.maxLevel = maxLevel;

                if (isRated)
                    ++arenaInfo.ratedArenaBotCount;
                else
                    ++arenaInfo.skirmishArenaBotCount;

                if (bg)
                {
                    std::vector<uint32>* instanceIds = nullptr;
                    if (bot->InArena() && bg->isRated())
                        instanceIds = &arenaInfo.ratedArenaInstances;
                    else if (bot->InArena())
                        instanceIds = &arenaInfo.skirmishArenaInstances;

                    if (instanceIds && std::find(instanceIds->begin(), instanceIds->end(), bg->GetInstanceID()) == instanceIds->end())
                        instanceIds->push_back(bg->GetInstanceID());

                    if (bot->InArena() && bg->isRated())
                        arenaInfo.ratedArenaInstanceCount = arenaInfo.ratedArenaInstances.size();
                    else if (bot->InArena())
                        arenaInfo.skirmishArenaInstanceCount = arenaInfo.skirmishArenaInstances.size();
                }

                continue;
            }

            bool queueState = bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
            bool activeState = bg && bg->GetBgTypeID() == BattlegroundMgr::BGTemplateId(queueTypeId);
            rtgRecordBgParticipant(guid, teamId, true, queueTypeId, bracketId, minLevel, maxLevel, queueState, activeState,
                                   activeState ? bg->GetInstanceID() : 0);
        }
    }

    // If enabled, wait for all bots to have logged in before queueing for Arena's / BG's
    if (sPlayerbotAIConfig.randomBotAutoJoinBG && playerBots.size() >= GetMaxAllowedBotCount())
    {
        uint32 randomBotAutoJoinArenaBracket = sPlayerbotAIConfig.randomBotAutoJoinArenaBracket;
        uint32 randomBotAutoJoinBGRatedArena2v2Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena2v2Count;
        uint32 randomBotAutoJoinBGRatedArena3v3Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena3v3Count;
        uint32 randomBotAutoJoinBGRatedArena5v5Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena5v5Count;

        uint32 randomBotAutoJoinBGICCount = sPlayerbotAIConfig.randomBotAutoJoinBGICCount;
        uint32 randomBotAutoJoinBGEYCount = sPlayerbotAIConfig.randomBotAutoJoinBGEYCount;
        uint32 randomBotAutoJoinBGAVCount = sPlayerbotAIConfig.randomBotAutoJoinBGAVCount;
        uint32 randomBotAutoJoinBGABCount = sPlayerbotAIConfig.randomBotAutoJoinBGABCount;
        uint32 randomBotAutoJoinBGWSCount = sPlayerbotAIConfig.randomBotAutoJoinBGWSCount;

        std::vector<uint32> icBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinICBrackets);
        std::vector<uint32> eyBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinEYBrackets);
        std::vector<uint32> avBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinAVBrackets);
        std::vector<uint32> abBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinABBrackets);
        std::vector<uint32> wsBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinWSBrackets);

        // Check both bgInstanceCount / bgInstances.size
        // to help counter against potentional inconsistencies
        auto updateRatedArenaInstanceCount = [&](uint32 queueType, uint32 bracket, uint32 minCount)
        {
            if (BattlegroundData[queueType][bracket].activeRatedArenaQueue == 0 &&
                BattlegroundData[queueType][bracket].ratedArenaInstanceCount < minCount &&
                BattlegroundData[queueType][bracket].ratedArenaInstances.size() < minCount)
                BattlegroundData[queueType][bracket].activeRatedArenaQueue = 1;
        };

        auto updateBGInstanceCount = [&](uint32 queueType, std::vector<uint32> brackets, uint32 minCount)
        {
            for (uint32 bracket : brackets)
            {
                if (BattlegroundData[queueType][bracket].activeBgQueue == 0 &&
                    BattlegroundData[queueType][bracket].bgInstanceCount < minCount &&
                    BattlegroundData[queueType][bracket].bgInstances.size() < minCount)
                    BattlegroundData[queueType][bracket].activeBgQueue = 1;
            }
        };

        // Update rated arena instance counts
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_2v2, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena2v2Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_3v3, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena3v3Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_5v5, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena5v5Count);

        // Update battleground instance counts
        updateBGInstanceCount(BATTLEGROUND_QUEUE_IC, icBrackets, randomBotAutoJoinBGICCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_EY, eyBrackets, randomBotAutoJoinBGEYCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AV, avBrackets, randomBotAutoJoinBGAVCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AB, abBrackets, randomBotAutoJoinBGABCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_WS, wsBrackets, randomBotAutoJoinBGWSCount);
    }

    LogBattlegroundInfo();
}

void RandomPlayerbotMgr::LogBattlegroundInfo()
{
    for (auto const& queueTypePair : BattlegroundData)
    {
        uint8 queueType = queueTypePair.first;

        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueType);

        if (uint8 type = BattlegroundMgr::BGArenaType(queueTypeId))
        {
            for (auto const& bracketIdPair : queueTypePair.second)
            {
                auto& bgInfo = bracketIdPair.second;
                if (bgInfo.minLevel == 0)
                    continue;
                LOG_INFO("playerbots",
                         "ARENA:{} {}: Player (Skirmish:{}, Rated:{}) Bots (Skirmish:{}, Rated:{}) Total (Skirmish:{} "
                         "Rated:{}), Instances (Skirmish:{} Rated:{})",
                         type == ARENA_TYPE_2v2   ? "2v2"
                         : type == ARENA_TYPE_3v3 ? "3v3"
                                                  : "5v5",
                         std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                         bgInfo.skirmishArenaPlayerCount, bgInfo.ratedArenaPlayerCount, bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaPlayerCount + bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaPlayerCount + bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaInstanceCount,
                         bgInfo.ratedArenaInstanceCount);
            }
            continue;
        }

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        std::string _bgType;
        switch (bgTypeId)
        {
            case BATTLEGROUND_AV:
                _bgType = "AV";
                break;
            case BATTLEGROUND_WS:
                _bgType = "WSG";
                break;
            case BATTLEGROUND_AB:
                _bgType = "AB";
                break;
            case BATTLEGROUND_EY:
                _bgType = "EotS";
                break;
            case BATTLEGROUND_RB:
                _bgType = "Random";
                break;
            case BATTLEGROUND_SA:
                _bgType = "SotA";
                break;
            case BATTLEGROUND_IC:
                _bgType = "IoC";
                break;
            default:
                _bgType = "Other";
                break;
        }

        for (auto const& bracketIdPair : queueTypePair.second)
        {
            auto& bgInfo = bracketIdPair.second;
            if (bgInfo.minLevel == 0)
                continue;

            LOG_INFO("playerbots",
                     "BG:{} {}: Player ({}:{}) Bot ({}:{}) Total (A:{} H:{}), Instances {}, Active Queue: {}", _bgType,
                     std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                     bgInfo.bgAlliancePlayerCount, bgInfo.bgHordePlayerCount, bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordeBotCount, bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount, bgInfo.bgInstanceCount, bgInfo.activeBgQueue);
        }
    }
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        RTG::RtgBgQueuePlanner bgPlanner;
        bgPlanner.ApplyDemandEvents(*this);
    }

    if (RTG_QueueDebugEnabled())
        LOG_INFO("playerbots",
    "[RTGDBG][BG] check end anyRealDemand={} needTotal={} start={} turnBg={}",
    GetEventValue(0, "rtg_bg_any_real_demand"),
    GetEventValue(0, "rtg_bg_need_total"),
    GetEventValue(0, "rtg_bg_start"),
    GetEventValue(0, "rtg_queue_turn_bg"));
    LOG_DEBUG("playerbots", "BG Queue check finished");
}

void RandomPlayerbotMgr::CheckLfgQueue()
{
    if (sPlayerbotAIConfig.rtgEventDriven && GetOnlineRealPlayerCount() == 0)
    {
        SetEventValue(0, "rtg_lfg_need_total", 0, 0);
        SetEventValue(0, "rtg_lfg_start", 0, 0);

        if (RTG_QueueDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFG] skip check: no real players online");
        return;
    }

    if (!LfgCheckTimer || time(nullptr) > (LfgCheckTimer + 30))
        LfgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking LFG Queue...");
    if (RTG_QueueDebugEnabled())
        LOG_INFO("playerbots", "[RTGDBG][LFG] check begin trackedPlayers={} trackedBots={}", static_cast<uint32>(players.size()), static_cast<uint32>(playerBots.size()));

    struct QueueRequest
    {
        uint32 owner = 0;
        uint32 team = 0;
        uint32 level = 0;
        uint32 realTank = 0;
        uint32 realHeal = 0;
        uint32 realDps = 0;
        uint32 realQueued = 0;
        uint32 realActive = 0;
        bool activeDungeon = false;
    };

    std::map<uint32, QueueRequest> requests;
    bool anyRealLfgDemand = false;

    LfgDungeons[TEAM_ALLIANCE].clear();
    LfgDungeons[TEAM_HORDE].clear();

    for (Player* player : players)
    {
        if (!player || !player->IsInWorld() || IsRandomBot(player))
            continue;

		Group* group = player->GetGroup();
		ObjectGuid queueGuid = player->GetGUID();

		if (group)
		{
			lfg::LfgState groupState = sLFGMgr->GetState(group->GetGUID());
			if (group->isLFGGroup() || groupState != lfg::LFG_STATE_NONE)
				queueGuid = group->GetGUID();
		}

        lfg::LfgState gState = sLFGMgr->GetState(queueGuid);
        Map* map = player->GetMap();
        bool queuedLfg = (gState != lfg::LFG_STATE_NONE && gState < lfg::LFG_STATE_DUNGEON);
        bool activeDungeon = (group && group->isLFGGroup()) ||
                             gState == lfg::LFG_STATE_DUNGEON ||
                             (map && (map->IsDungeon() || map->IsRaid()) && group && group->isLFGGroup());
        if (!queuedLfg && !activeDungeon)
            continue;

        anyRealLfgDemand = true;

        uint32 owner = queueGuid.GetCounter();
        QueueRequest& req = requests[owner];
        req.owner = owner;
        req.team = player->GetTeamId();
        req.level = player->GetLevel();
        req.activeDungeon = req.activeDungeon || activeDungeon;
        if (queuedLfg)
            ++req.realQueued;
        if (activeDungeon)
            ++req.realActive;

        uint32 role = RTG_NormalizeQueuedRoleMask(sLFGMgr->GetRoles(player->GetGUID()));
        if (!role)
            role = RTG_DefaultRoleForClass(player->getClass());

        if (role == lfg::PLAYER_ROLE_TANK)
            ++req.realTank;
        else if (role == lfg::PLAYER_ROLE_HEALER)
            ++req.realHeal;
        else
            ++req.realDps;

        if (queuedLfg)
        {
            lfg::LfgDungeonSet const& dList = sLFGMgr->GetSelectedDungeons(player->GetGUID());
            for (lfg::LfgDungeonSet::const_iterator itr = dList.begin(); itr != dList.end(); ++itr)
            {
                lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(*itr);
                if (!dungeon)
                    continue;

                LfgDungeons[player->GetTeamId()].push_back(dungeon->id);
            }
        }
    }
	
	// Owner-local LFG demand should expire quickly so stale real-player groups
	// do not keep helper bots alive too long after queue/group collapse.
	// Global LFG demand can live longer to avoid thrashing between scans.
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 now = static_cast<uint32>(time(nullptr));
		uint32 ownerTtl = sPlayerbotAIConfig.rtgQueueGraceSeconds + 20;
		uint32 globalTtl = sPlayerbotAIConfig.rtgQueueGraceSeconds + 120;
		uint32 desiredHelperTotal = 0;
		bool anyReady = false;
		uint32 oldestPendingStart = 0;

        for (auto const& kv : requests)
        {
            QueueRequest const& req = kv.second;
            uint32 existingStart = GetEventValue(req.owner, "rtg_lfg_start");
            uint32 startTs = req.activeDungeon ? (now - sPlayerbotAIConfig.rtgQueueGraceSeconds) : (existingStart ? existingStart : now);
            SetEventValue(req.owner, "rtg_lfg_start", startTs, ownerTtl, RTG::MakeLfgAddData(req.team, req.level, 0, req.owner));
			SetEventValue(req.owner, "rtg_lfg_real_demand", 1u, ownerTtl, RTG::MakeLfgAddData(req.team, req.level, 0, req.owner));
            
			uint32 needTank = req.realTank >= 1 ? 0u : 1u;
            uint32 needHeal = req.realHeal >= 1 ? 0u : 1u;
            uint32 needDps = req.realDps >= 3 ? 0u : (3u - req.realDps);
            uint32 helperNeed = needTank + needHeal + needDps;
            desiredHelperTotal += helperNeed;

            if (RTG_QueueDebugEnabled() || helperNeed)
            {
                LOG_INFO("playerbots", "[RTG][LFG][PLAN] owner={} team={} level={} realQueued={} realActive={} needTank={} needHeal={} needDps={} startTs={}",
                         req.owner, req.team, req.level, req.realQueued, req.realActive, needTank, needHeal, needDps, startTs);
            }

            if (req.activeDungeon || now >= startTs + sPlayerbotAIConfig.rtgQueueGraceSeconds)
                anyReady = true;
            else if (!oldestPendingStart || startTs < oldestPendingStart)
                oldestPendingStart = startTs;
        }

        if (anyRealLfgDemand && desiredHelperTotal)
        {
            uint32 globalStart = anyReady ? (now - sPlayerbotAIConfig.rtgQueueGraceSeconds) : (oldestPendingStart ? oldestPendingStart : now);
            uint32 cappedNeed = std::min<uint32>(desiredHelperTotal, sPlayerbotAIConfig.rtgLfgMaxBots);
            LOG_INFO("playerbots", "[RTG][LFG][TOTAL] demandOwners={} desiredHelpers={} cappedHelpers={} anyReady={} globalStart={}",
                     static_cast<uint32>(requests.size()), desiredHelperTotal, cappedNeed, anyReady ? 1u : 0u, globalStart);
            SetEventValue(0, "rtg_lfg_start", globalStart, globalTtl);
			SetEventValue(0, "rtg_lfg_need_total", cappedNeed, globalTtl);
        }
        else
        {
            SetEventValue(0, "rtg_lfg_start", 0, 0);
            SetEventValue(0, "rtg_lfg_need_total", 0, 0);
        }
    }

    if (RTG_QueueDebugEnabled())
        LOG_INFO("playerbots", "[RTGDBG][LFG] check end owners={} anyRealDemand={} needTotal={} start={}", static_cast<uint32>(requests.size()), anyRealLfgDemand ? 1u : 0u, GetEventValue(0, "rtg_lfg_need_total"), GetEventValue(0, "rtg_lfg_start"));
    LOG_DEBUG("playerbots", "LFG Queue check finished");
}

void RandomPlayerbotMgr::CheckPlayers()
{
    if (!PlayersCheckTimer || time(nullptr) > (PlayersCheckTimer + 60))
        PlayersCheckTimer = time(nullptr);

    LOG_INFO("playerbots", "Checking Players...");

    if (!playersLevel)
        playersLevel = sPlayerbotAIConfig.randombotStartingLevel;

    for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
    {
        Player* player = *i;

        if (player->IsGameMaster())
            continue;

        // if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        //     continue;

        if (player->GetLevel() > playersLevel)
            playersLevel = player->GetLevel() + 3;
    }

    LOG_INFO("playerbots", "Max player level is {}, max bot level set to {}", playersLevel - 3, playersLevel);
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time) { SetEventValue(bot, "randomize", 1, time); }

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot, uint32 time)
{
    if (!time)
        time = 60 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3);

    SetEventValue(bot, "teleport", 1, time);
}

void RandomPlayerbotMgr::ScheduleChangeStrategy(uint32 bot, uint32 time)
{
    if (!time)
        time = urand(sPlayerbotAIConfig.minRandomBotChangeStrategyTime,
                     sPlayerbotAIConfig.maxRandomBotChangeStrategyTime);

    SetEventValue(bot, "change_strategy", 1, time);
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    ObjectGuid botGUID = ObjectGuid::Create<HighGuid::Player>(bot);
    Player* player = GetPlayerBot(botGUID);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;

    uint32 isValid = GetEventValue(bot, "add");
    if (!isValid)
    {
        if (!player || !player->GetGroup())
        {
            if (player)
                LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                          player->GetLevel(), player->GetName().c_str());
            else
                LOG_DEBUG("playerbots", "Bot #{}: log out", bot);

            SetEventValue(bot, "add", 0, 0);
            currentBots.remove(bot);

            if (player)
                LogoutPlayerBot(botGUID);
        }

        return false;
    }

    uint32 randomTime;
    if (!player)
    {
        std::string addData = GetEventData(bot, "add");
        uint32 desiredTeam = 0;
        uint32 desiredLevel = 0;
        uint32 desiredRole = 0;
        uint32 desiredOwner = 0;
        if (RTG::ParseLfgAddData(addData, desiredTeam, desiredLevel, &desiredRole, &desiredOwner) && RTG_IsOwnerInBattlegroundMap(desiredOwner))
        {
            RTG_RuntimeBreadcrumb(fmt::format("[RTG][LFG][ABANDON] helper={} owner={} reason=owner_in_bg_map", bot, desiredOwner));
            SetEventValue(bot, "add", 0, 0);
            SetEventValue(bot, "logout", 0, 0);
            SetEventValue(bot, "rtg_add_requested", 0, 0);
            SetEventValue(bot, "rtg_lfg_pending", 0, 0);
            currentBots.remove(bot);
            if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
                RTG::RtgQueueLedger::Instance().Remove(bot);
            return false;
        }

        if (RTG::IsQueueManagedAddData(addData))
        {
            uint32 requestTs = GetEventValue(bot, "rtg_add_requested");
            uint32 nowTs = NowSeconds();
            uint32 stallThreshold = RTG_GetDispatchStallThresholdSeconds();
            if (requestTs && nowTs > requestTs && (nowTs - requestTs) >= stallThreshold)
            {
                RTG_RuntimeBreadcrumb(fmt::format("[RTG][DISPATCH][STALL] helper={} waited={} add='{}'", bot, (nowTs - requestTs), addData));
                SetEventValue(bot, "add", 0, 0);
                SetEventValue(bot, "logout", 0, 0);
                SetEventValue(bot, "rtg_add_requested", 0, 0);
                SetEventValue(bot, "rtg_lfg_pending", 0, 0);
                SetEventValue(bot, "rtg_bg_pending", 0, 0);
                SetEventValue(bot, "rtg_bg_queue_grace", 0, 0);
                SetEventValue(bot, "rtg_bg_queue_retry", 0, 0);
                currentBots.remove(bot);
                if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
                    RTG::RtgQueueLedger::Instance().Remove(bot);
                return false;
            }

            RTG_RuntimeBreadcrumb(fmt::format("[RTG][DISPATCH][ADD] helper={} add='{}'", bot, addData));
        }

        AddPlayerBot(botGUID, 0);
        randomTime = urand(1, 2);

        uint32 randomBotUpdateInterval = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
        randomTime = urand(std::max(5, static_cast<int>(randomBotUpdateInterval * 0.5)),
                           std::max(12, static_cast<int>(randomBotUpdateInterval * 2)));
        SetEventValue(bot, "update", 1, randomTime);

        // do not randomize or teleport immediately after server start (prevent lagging)
        if (!GetEventValue(bot, "randomize"))
        {
            randomTime = urand(3, std::max(4, static_cast<int>(randomBotUpdateInterval * 0.4)));
            ScheduleRandomize(bot, randomTime);
        }
        if (!GetEventValue(bot, "teleport"))
        {
            randomTime = urand(std::max(7, static_cast<int>(randomBotUpdateInterval * 0.7)),
                               std::max(14, static_cast<int>(randomBotUpdateInterval * 1.4)));
            ScheduleTeleport(bot, randomTime);
        }

        return true;
    }

    if (!player->IsInWorld())
        return false;

    if (player->GetGroup() || player->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    uint32 update = GetEventValue(bot, "update");
    if (!update)
    {
        if (botAI)
            botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);

        bool update = true;
        if (botAI)
        {
            // botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);
            if (!sRandomPlayerbotMgr.IsRandomBot(player))
                update = false;

            if (player->GetGroup() && botAI->GetGroupLeader())
            {
                PlayerbotAI* groupLeaderBotAI = GET_PLAYERBOT_AI(botAI->GetGroupLeader());
                if (!groupLeaderBotAI || groupLeaderBotAI->IsRealPlayer())
                {
                    update = false;
                }
            }

            // if (botAI->HasPlayerNearby(sPlayerbotAIConfig.grindDistance))
            //     update = false;
        }

        if (update)
            ProcessBot(player);

        randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);

        return true;
    }

    uint32 logout = GetEventValue(bot, "logout");
    if (player && !logout && !isValid)
    {
        LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                  player->GetLevel(), player->GetName().c_str());
        LogoutPlayerBot(botGUID);
        currentBots.remove(bot);
        SetEventValue(bot, "logout", 1,
                      urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        return true;
    }

    return false;
}

bool RandomPlayerbotMgr::ProcessBot(Player* bot)
{

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

     uint32 botId = bot->GetGUID().GetCounter();

    // if death revive
    if (bot->isDead())
    {
        if (!GetEventValue(botId, "dead"))
        {
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
            LOG_DEBUG("playerbots", "Mark bot {} as dead, will be revived in {}s.", bot->GetName().c_str(),
                      randomTime);
            SetEventValue(botId, "dead", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
            SetEventValue(botId, "revive", 1, randomTime);
            return false;
        }

        if (!GetEventValue(botId, "revive"))
        {
            Revive(bot);
            return true;
        }

        return false;
    }

    // leave group if leader is rndbot
    Group* group = bot->GetGroup();
    if (group && !group->isLFGGroup() && IsRandomBot(group->GetLeader()))
    {
        botAI->LeaveOrDisbandGroup();
        LOG_INFO("playerbots", "Bot {} remove from group since leader is random bot.", bot->GetName().c_str());
    }

    // only randomize and teleport idle bots
    bool idleBot = false;
    if (TravelTarget* target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
    {
        if (target->getTravelState() == TravelState::TRAVEL_STATE_IDLE)
        {
            idleBot = true;
        }
    }
    else
    {
        idleBot = true;
    }

    if (idleBot)
    {
        // randomize
        uint32 randomize = GetEventValue(botId, "randomize");
        if (!randomize)
        {
            // bool randomiser = true;
            // if (player->GetGuildId())
            // {
            //     if (Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId()))
            //     {
            //         if (guild->GetLeaderGUID() == player->GetGUID())
            //         {
            //             for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
            //                 sGuildTaskMgr->Update(*i, player);
            //         }

            //         uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guild->GetLeaderGUID());
            //         if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            //         {
            //             uint8 rank = player->GetRank();
            //             randomiser = rank < 4 ? false : true;
            //         }
            //     }
            // }
            // if (randomiser)
            // {
            Randomize(bot);
            LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: randomized", botId,
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
            ScheduleRandomize(botId, randomTime);
            return true;
        }

        // uint32 changeStrategy = GetEventValue(bot, "change_strategy");
        // if (!changeStrategy)
        // {
        //     LOG_INFO("playerbots", "Changing strategy for bot  #{} <{}>", bot, player->GetName().c_str());
        //     ChangeStrategy(player);
        //     return true;
        // }

        uint32 teleport = GetEventValue(botId, "teleport");
        if (!teleport)
        {
            LOG_DEBUG("playerbots", "Bot #{} <{}>: teleport for level and refresh", botId, bot->GetName());
            Refresh(bot);
            RandomTeleportForLevel(bot);
            uint32 time = urand(sPlayerbotAIConfig.minRandomBotTeleportInterval,
                                sPlayerbotAIConfig.maxRandomBotTeleportInterval);
            ScheduleTeleport(botId, time);
            return true;
        }
    }

    return false;
}

void RandomPlayerbotMgr::Revive(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    // LOG_INFO("playerbots", "Bot {} revived", player->GetName().c_str());
    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    Refresh(player);
    RandomTeleportGrindForLevel(player);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth)
{
    // ignore when alrdy teleported or not in the world yet.
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    // no teleport / movement update when rooted.
    if (bot->IsRooted())
        return;

    // ignore when in queue for battle grounds.
    if (bot->InBattlegroundQueue())
        return;

    // ignore when in battle grounds or arena.
    if (bot->InBattleground() || bot->InArena())
        return;

    // ignore when in group (e.g. world, dungeons, raids) and leader is not a player.
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (botAI)
    {
        // ignore when in when taxi with boat/zeppelin and has players nearby
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && bot->HasUnitState(UNIT_STATE_IGNORE_PATHFINDING) &&
            botAI->HasPlayerNearby())
            return;
    }

    // if (sPlayerbotAIConfig.randomBotRpgChance < 0)
    //     return;

    if (locs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - no locations available", bot->GetName().c_str());
        return;
    }

    std::vector<WorldPosition> tlocs;
    for (auto& loc : locs)
        tlocs.push_back(WorldPosition(loc));
    // Do not teleport to maps disabled in config
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(),
                               [bot](WorldPosition l)
                               {
                                   std::vector<uint32>::iterator i =
                                       find(sPlayerbotAIConfig.randomBotMaps.begin(),
                                            sPlayerbotAIConfig.randomBotMaps.end(), l.getMapId());
                                   return i == sPlayerbotAIConfig.randomBotMaps.end();
                               }),
                tlocs.end());
    if (tlocs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - all locations removed by filter", bot->GetName().c_str());
        return;
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleportByLocations");

    std::shuffle(std::begin(tlocs), std::end(tlocs), RandomEngine::Instance());
    for (uint32 i = 0; i < tlocs.size(); i++)
    {
        WorldLocation loc = tlocs[i];

        float x = loc.GetPositionX();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float y = loc.GetPositionY();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float z = loc.GetPositionZ();

        Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
        if (!map)
            continue;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(map->GetZoneId(bot->GetPhaseMask(), x, y, z));
        if (!zone)
            continue;

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(bot->GetPhaseMask(), x, y, z));
        if (!area)
            continue;

        // Do not teleport to enemy zones if level is low
        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        float ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
        if (ground <= INVALID_HEIGHT)
            continue;

        z = 0.05f + ground;

        if (!botAI->CheckLocationDistanceByLevel(bot, loc, true))
            continue;

        const LocaleConstant& locale = sWorld->GetDefaultDbcLocale();
        LOG_DEBUG("playerbots",
                  "Random teleporting bot {} (level {}) to Map: {} ({}) Zone: {} ({}) Area: {} ({}) ZoneLevel: {} "
                  "AreaLevel: {} {},{},{} ({}/{} "
                  "locations)",
                  bot->GetName().c_str(), bot->GetLevel(), map->GetId(), map->GetMapName(), zone->ID,
                  zone->area_name[locale], area->ID, area->area_name[locale], zone->area_level, area->area_level, x, y,
                  z, i + 1, tlocs.size());

        if (hearth)
        {
            bot->SetHomebind(loc, zone->ID);
        }

        // Prevent blink to be detected by visible real players
        if (botAI->HasPlayerNearby(150.0f))
        {
            break;
        }

        bot->GetMotionMaster()->Clear();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI)
            botAI->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(loc.GetMapId(), x, y, z, 0);
        bot->SendMovementFlagUpdate();

        if (pmo)
            pmo->finish();

        return;
    }

    if (pmo)
        pmo->finish();

    // LOG_ERROR("playerbots", "Cannot teleport bot {} - no locations available ({} locations)", bot->GetName().c_str(),
    //           tlocs.size());
}

void RandomPlayerbotMgr::PrepareZone2LevelBracket()
{
    // Classic WoW - Low - level zones
    zone2LevelBracket[1] = {5, 12};     // Dun Morogh
    zone2LevelBracket[12] = {5, 12};    // Elwynn Forest
    zone2LevelBracket[14] = {5, 12};    // Durotar
    zone2LevelBracket[85] = {5, 12};    // Tirisfal Glades
    zone2LevelBracket[141] = {5, 12};   // Teldrassil
    zone2LevelBracket[215] = {5, 12};   // Mulgore
    zone2LevelBracket[3430] = {5, 12};  // Eversong Woods
    zone2LevelBracket[3524] = {5, 12};  // Azuremyst Isle

    // Classic WoW - Mid - level zones
    zone2LevelBracket[17] = {10, 25};    // Barrens
    zone2LevelBracket[38] = {10, 20};    // Loch Modan
    zone2LevelBracket[40] = {10, 21};    // Westfall
    zone2LevelBracket[130] = {10, 23};   // Silverpine Forest
    zone2LevelBracket[148] = {10, 21};   // Darkshore
    zone2LevelBracket[3433] = {10, 22};  // Ghostlands
    zone2LevelBracket[3525] = {10, 21};  // Bloodmyst Isle

    // Classic WoW - High - level zones
    zone2LevelBracket[10] = {19, 33};   // Deadwind Pass
    zone2LevelBracket[11] = {21, 30};   // Wetlands
    zone2LevelBracket[44] = {16, 28};   // Redridge Mountains
    zone2LevelBracket[267] = {20, 34};  // Hillsbrad Foothills
    zone2LevelBracket[331] = {18, 33};  // Ashenvale
    zone2LevelBracket[400] = {24, 36};  // Thousand Needles
    zone2LevelBracket[406] = {16, 29};  // Stonetalon Mountains

    // Classic WoW - Higher - level zones
    zone2LevelBracket[3] = {36, 46};    // Badlands
    zone2LevelBracket[8] = {36, 46};    // Swamp of Sorrows
    zone2LevelBracket[15] = {35, 46};   // Dustwallow Marsh
    zone2LevelBracket[16] = {45, 52};   // Azshara
    zone2LevelBracket[33] = {32, 47};   // Stranglethorn Vale
    zone2LevelBracket[45] = {30, 42};   // Arathi Highlands
    zone2LevelBracket[47] = {42, 51};   // Hinterlands
    zone2LevelBracket[51] = {45, 51};   // Searing Gorge
    zone2LevelBracket[357] = {40, 52};  // Feralas
    zone2LevelBracket[405] = {30, 41};  // Desolace
    zone2LevelBracket[440] = {41, 52};  // Tanaris

    // Classic WoW - Top - level zones
    zone2LevelBracket[4] = {52, 57};     // Blasted Lands
    zone2LevelBracket[28] = {50, 60};    // Western Plaguelands
    zone2LevelBracket[46] = {51, 60};    // Burning Steppes
    zone2LevelBracket[139] = {54, 62};   // Eastern Plaguelands
    zone2LevelBracket[361] = {47, 57};   // Felwood
    zone2LevelBracket[490] = {49, 56};   // Un'Goro Crater
    zone2LevelBracket[618] = {54, 61};   // Winterspring
    zone2LevelBracket[1377] = {54, 63};  // Silithus

    // The Burning Crusade - Zones
    zone2LevelBracket[3483] = {58, 66};  // Hellfire Peninsula
    zone2LevelBracket[3518] = {64, 70};  // Nagrand
    zone2LevelBracket[3519] = {62, 73};  // Terokkar Forest
    zone2LevelBracket[3520] = {66, 73};  // Shadowmoon Valley
    zone2LevelBracket[3521] = {60, 67};  // Zangarmarsh
    zone2LevelBracket[3522] = {64, 73};  // Blade's Edge Mountains
    zone2LevelBracket[3523] = {67, 73};  // Netherstorm
    zone2LevelBracket[4080] = {68, 73};  // Isle of Quel'Danas

    // Wrath of the Lich King - Zones
    zone2LevelBracket[65] = {71, 77};    // Dragonblight
    zone2LevelBracket[66] = {74, 80};    // Zul'Drak
    zone2LevelBracket[67] = {77, 80};    // Storm Peaks
    zone2LevelBracket[210] = {77, 80};   // Icecrown Glacier
    zone2LevelBracket[394] = {72, 78};   // Grizzly Hills
    zone2LevelBracket[495] = {68, 74};   // Howling Fjord
    zone2LevelBracket[2817] = {77, 80};  // Crystalsong Forest
    zone2LevelBracket[3537] = {68, 75};  // Borean Tundra
    zone2LevelBracket[3711] = {75, 80};  // Sholazar Basin
    zone2LevelBracket[4197] = {79, 80};  // Wintergrasp

    // Override with values from config
    for (auto const& [zoneId, bracketPair] : sPlayerbotAIConfig.zoneBrackets)
    {
        zone2LevelBracket[zoneId] = {bracketPair.first, bracketPair.second};
    }
}

void RandomPlayerbotMgr::PrepareTeleportCache()
{
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    LOG_INFO("playerbots", "Preparing random teleport caches for {} levels...", maxLevel);

    QueryResult results = WorldDatabase.Query(
        "SELECT "
        "g.map, "
        "position_x, "
        "position_y, "
        "position_z, "
        "t.minlevel, "
        "t.maxlevel "
        "FROM "
        "(SELECT "
        "map, "
        "MIN( c.guid ) guid "
        "FROM "
        "creature c "
        "INNER JOIN creature_template t ON c.id1 = t.entry "
        "WHERE "
        "t.npcflag = 0 "
        "AND t.lootid != 0 "
        "AND t.maxlevel - t.minlevel < 3 "
        "AND map IN ({}) "
        "AND t.entry not in (32820, 24196, 30627, 30617) "
        "AND c.spawntimesecs < 1000 "
        "AND t.faction not in (11, 71, 79, 85, 188, 1575) "
        "AND (t.unit_flags & 256) = 0 "
        "AND (t.unit_flags & 4096) = 0 "
        "AND t.rank = 0 "
        // "AND (t.flags_extra & 32768) = 0 "
        "GROUP BY "
        "map, "
        "ROUND(position_x / 50), "
        "ROUND(position_y / 50), "
        "ROUND(position_z / 50) "
        "HAVING "
        "count(*) >= 2) "
        "AS g "
        "INNER JOIN creature c ON g.guid = c.guid "
        "INNER JOIN creature_template t on c.id1 = t.entry "
        "ORDER BY "
        "t.minlevel;",
        sPlayerbotAIConfig.randomBotMapsAsString.c_str());
    uint32 collected_locs = 0;
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint16 mapId = fields[0].Get<uint16>();
            float x = fields[1].Get<float>();
            float y = fields[2].Get<float>();
            float z = fields[3].Get<float>();
            uint32 min_level = fields[4].Get<uint32>();
            uint32 max_level = fields[5].Get<uint32>();
            uint32 level = (min_level + max_level + 1) / 2;
            WorldLocation loc(mapId, x, y, z, 0);
            collected_locs++;
            for (int32 l = (int32)level - (int32)sPlayerbotAIConfig.randomBotTeleLowerLevel;
                 l <= (int32)level + (int32)sPlayerbotAIConfig.randomBotTeleHigherLevel; l++)
            {
                if (l < 1 || l > maxLevel)
                {
                    continue;
                }
                locsPerLevelCache[(uint8)l].push_back(loc);
            }
        } while (results->NextRow());
    }
    LOG_INFO("playerbots", ">> {} locations for level collected.", collected_locs);

    if (sPlayerbotAIConfig.enableNewRpgStrategy)
    {
        PrepareZone2LevelBracket();
        LOG_INFO("playerbots", "Preparing innkeepers / flightmasters locations for level...");
        results = WorldDatabase.Query(
            "SELECT "
            "map, "
            "position_x, "
            "position_y, "
            "position_z, "
            "orientation, "
            "t.faction, "
            "t.entry, "
            "t.npcflag, "
            "c.guid "
            "FROM "
            "creature c "
            "INNER JOIN creature_template t on c.id1 = t.entry "
            "WHERE "
            "t.npcflag & 73728 "
            "AND map IN ({}) "
            "ORDER BY "
            "t.minlevel;",
            sPlayerbotAIConfig.randomBotMapsAsString.c_str());
        collected_locs = 0;
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                uint16 mapId = fields[0].Get<uint16>();
                float x = fields[1].Get<float>();
                float y = fields[2].Get<float>();
                float z = fields[3].Get<float>();
                float orient = fields[4].Get<float>();
                uint32 faction = fields[5].Get<uint32>();
                uint32 tEntry = fields[6].Get<uint32>();
                uint32 tNpcflag = fields[7].Get<uint32>();
                uint32 guid = fields[8].Get<uint32>();

                if (tEntry == 3838 || tEntry == 29480)
                    continue;

                const FactionTemplateEntry* entry = sFactionTemplateStore.LookupEntry(faction);

                WorldLocation loc(mapId, x + cos(orient) * 5.0f, y + sin(orient) * 5.0f, z + 0.5f, orient + M_PI);
                collected_locs++;
                Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
                if (!map)
                    continue;
                bool forHorde = !(entry->hostileMask & 4);
                bool forAlliance = !(entry->hostileMask & 2);
                if (tNpcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
                {
                    WorldPosition pos(mapId, x, y, z, orient);
                    if (forHorde)
                        sFlightMasterCache.AddHordeFlightMaster(guid, pos);

                    if (forAlliance)
                        sFlightMasterCache.AddAllianceFlightMaster(guid, pos);
                }
                const AreaTableEntry* area = sAreaTableStore.LookupEntry(map->GetAreaId(PHASEMASK_NORMAL, x, y, z));
                uint32 zoneId = area->zone ? area->zone : area->ID;
                if (zone2LevelBracket.find(zoneId) == zone2LevelBracket.end())
                    continue;
                LevelBracket bracket = zone2LevelBracket[zoneId];
                for (int i = bracket.low; i <= bracket.high; i++)
                {
                    if (forHorde)
                    {
                        hordeStarterPerLevelCache[i].push_back(loc);
                    }
                    if (forAlliance)
                    {
                        allianceStarterPerLevelCache[i].push_back(loc);
                    }
                }

            } while (results->NextRow());
        }

        // add all initial position
        for (uint32 i = 1; i < MAX_RACES; i++)
        {
            for (uint32 j = 1; j < MAX_CLASSES; j++)
            {
                PlayerInfo const* info = sObjectMgr->GetPlayerInfo(i, j);

                if (!info)
                    continue;

                WorldPosition pos(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);

                for (int32 l = 1; l <= 5; l++)
                {
                    if ((1 << (i - 1)) & RACEMASK_ALLIANCE)
                        allianceStarterPerLevelCache[(uint8)l].push_back(pos);
                    else
                        hordeStarterPerLevelCache[(uint8)l].push_back(pos);
                }
                break;
            }
        }
        LOG_INFO("playerbots", ">> {} innkeepers locations for level collected.", collected_locs);
    }

    results = WorldDatabase.Query(
        "SELECT "
        "map, "
        "position_x, "
        "position_y, "
        "position_z, "
        "orientation, "
        "t.minlevel, "
        "t.entry "
        "FROM "
        "creature c "
        "INNER JOIN creature_template t on c.id1 = t.entry "
        "WHERE "
        "t.npcflag & 131072 "
        "AND t.npcflag != 135298 "
        "AND t.minlevel != 55 "
        "AND t.minlevel != 65 "
        "AND t.faction not in (35, 474, 69, 57) "
        "AND t.entry not in (30606, 30608, 29282) "
        "AND map IN ({}) "
        "ORDER BY "
        "t.minlevel;",
        sPlayerbotAIConfig.randomBotMapsAsString.c_str());
    collected_locs = 0;
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint16 mapId = fields[0].Get<uint16>();
            float x = fields[1].Get<float>();
            float y = fields[2].Get<float>();
            float z = fields[3].Get<float>();
            float orient = fields[4].Get<float>();
            uint32 level = fields[5].Get<uint32>();
            uint32 entry = fields[6].Get<uint32>();
            BankerLocation bLoc;
            bLoc.loc = WorldLocation(mapId, x + cos(orient) * 6.0f, y + sin(orient) * 6.0f, z + 2.0f, orient + M_PI);
            bLoc.entry = entry;
            collected_locs++;
            for (int32 l = 1; l <= maxLevel; l++)
            {
                // Bots 1-60 go to base game bankers (all have minlevel 30 or 45)
                if (l <=60 && level > 45)
                {
                    continue;
                }
                // Bots 61-70 go to Shattrath bankers (all have minlevel 60 or 70)
                if ((l >=61 && l <=70) && (level < 60 || level > 70))
                {
                    continue;
                }
                // Bots 71+ go to Dalaran bankers (all have minlevel 75)
                if ((l >=71) && level != 75)
                {
                    continue;
                }
                bankerLocsPerLevelCache[(uint8)l].push_back(bLoc);
                bankerEntryToLocation[bLoc.entry] = bLoc.loc;
            }
        } while (results->NextRow());
    }
    LOG_INFO("playerbots", ">> {} banker locations for level collected.", collected_locs);
}

void RandomPlayerbotMgr::PrepareAddclassCache()
{
    // Using accounts marked as type 2 (AddClass)
    int32 collected = 0;

    for (uint32 accountId : addClassTypeAccounts)
    {
        for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
        {
            if (claz == 10)
                continue;

            QueryResult results = CharacterDatabase.Query(
                "SELECT guid, race FROM characters "
                "WHERE account = {} AND class = '{}' AND online = 0",
                accountId, claz);

            if (results)
            {
                do
                {
                    Field* fields = results->Fetch();
                    ObjectGuid guid = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
                    uint32 race = fields[1].Get<uint32>();
                    bool isAlliance = race == 1 || race == 3 || race == 4 || race == 7 || race == 11;
                    addclassCache[GetTeamClassIdx(isAlliance, claz)].insert(guid);
                    collected++;
                } while (results->NextRow());
            }
        }
    }

    LOG_INFO("playerbots", ">> {} characters collected for addclass command from {} AddClass accounts.", collected, addClassTypeAccounts.size());
}

void RandomPlayerbotMgr::Init()
{
    if (sPlayerbotAIConfig.addClassCommand)
        sRandomPlayerbotMgr.PrepareAddclassCache();

    if (sPlayerbotAIConfig.enabled)
    {
        sRandomPlayerbotMgr.PrepareTeleportCache();
    }

    if (sPlayerbotAIConfig.randomBotJoinBG)
        sRandomPlayerbotMgr.LoadBattleMastersCache();

    PlayerbotsDatabase.Execute("DELETE FROM playerbots_random_bots WHERE event = 'add'");
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    uint32 level = bot->GetLevel();
    uint8 race = bot->getRace();
    std::vector<WorldLocation>* locs = nullptr;
    if (sPlayerbotAIConfig.enableNewRpgStrategy)
        locs = IsAlliance(race) ? &allianceStarterPerLevelCache[level] : &hordeStarterPerLevelCache[level];
    else
        locs = &locsPerLevelCache[level];
    if (level >= 10 && urand(0, 100) < sPlayerbotAIConfig.probTeleToBankers * 100)
    {
        std::vector<WorldLocation> fallbackLocs;
        for (auto& bLoc : bankerLocsPerLevelCache[level])
            fallbackLocs.push_back(bLoc.loc);

        if (!sPlayerbotAIConfig.enableWeightTeleToCityBankers)
        {
            RandomTeleport(bot, fallbackLocs, true);
            return;
        }

        // Collect valid cities based on bot faction.
        std::unordered_set<CityId> validBankerCities;
        for (auto& loc : bankerLocsPerLevelCache[level])
        {
            auto cityIt = bankerToCity.find(loc.entry);
            if (cityIt == bankerToCity.end()) continue;

            CityId cityId = cityIt->second.first;
            FactionId cityFactionId = cityIt->second.second;

            if ((IsAlliance(bot->getRace()) && cityFactionId == FactionId::ALLIANCE) ||
                (!IsAlliance(bot->getRace()) && cityFactionId == FactionId::HORDE) ||
                (cityFactionId == FactionId::NEUTRAL))
            {
                validBankerCities.insert(cityId);
            }
        }

        // Fallback if no valid cities
        if (validBankerCities.empty())
        {
            RandomTeleport(bot, fallbackLocs, true);
            return;
        }

        // Apply weights to valid cities
        std::vector<CityId> weightedCities;
        for (CityId city : validBankerCities)
        {
            int weight = 0;
            switch (city)
            {
                case CityId::STORMWIND:       weight = sPlayerbotAIConfig.weightTeleToStormwind; break;
                case CityId::IRONFORGE:       weight = sPlayerbotAIConfig.weightTeleToIronforge; break;
                case CityId::DARNASSUS:       weight = sPlayerbotAIConfig.weightTeleToDarnassus; break;
                case CityId::EXODAR:          weight = sPlayerbotAIConfig.weightTeleToExodar; break;
                case CityId::ORGRIMMAR:       weight = sPlayerbotAIConfig.weightTeleToOrgrimmar; break;
                case CityId::UNDERCITY:       weight = sPlayerbotAIConfig.weightTeleToUndercity; break;
                case CityId::THUNDER_BLUFF:   weight = sPlayerbotAIConfig.weightTeleToThunderBluff; break;
                case CityId::SILVERMOON_CITY: weight = sPlayerbotAIConfig.weightTeleToSilvermoonCity; break;
                case CityId::SHATTRATH_CITY:  weight = sPlayerbotAIConfig.weightTeleToShattrathCity; break;
                case CityId::DALARAN:         weight = sPlayerbotAIConfig.weightTeleToDalaran; break;
                default:              weight = 0; break;
            }
            if (weight <= 0) continue;

            for (int i = 0; i < weight; ++i)
            {
                weightedCities.push_back(city);
            }
        }

        // Fallback if no valid cities
        if (weightedCities.empty())
        {
            RandomTeleport(bot, fallbackLocs, true);
            return;
        }

        // Pick a weighted city randomly, then a random banker in that city
        //   then teleport to that banker
        CityId selectedCity = weightedCities[urand(0, weightedCities.size() - 1)];
        auto const& bankers = cityToBankers.at(selectedCity);
        uint32 selectedBankerEntry = bankers[urand(0, bankers.size() - 1)];
        auto locIt = bankerEntryToLocation.find(selectedBankerEntry);
        if (locIt != bankerEntryToLocation.end())
        {
            std::vector<WorldLocation> teleportTarget = { locIt->second };
            RandomTeleport(bot, teleportTarget, true);
            return;
        }

        // Fallback if something went wrong
        RandomTeleport(bot, *locs);
    }
    else
    {
        RandomTeleport(bot, *locs);
    }
}

void RandomPlayerbotMgr::RandomTeleportGrindForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    uint32 level = bot->GetLevel();
    uint8 race = bot->getRace();
    std::vector<WorldLocation>* locs = nullptr;
    if (sPlayerbotAIConfig.enableNewRpgStrategy)
        locs = IsAlliance(race) ? &allianceStarterPerLevelCache[level] : &hordeStarterPerLevelCache[level];
    else
        locs = &locsPerLevelCache[level];
    LOG_DEBUG("playerbots", "Random teleporting bot {} for level {} ({} locations available)", bot->GetName().c_str(),
              bot->GetLevel(), locs->size());

    RandomTeleport(bot, *locs);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot)
{
    if (bot->InBattleground())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleport");
    std::vector<WorldLocation> locs;

    std::list<Unit*> targets;
    float range = sPlayerbotAIConfig.randomBotTeleportDistance;
    Acore::AnyUnitInObjectRangeCheck u_check(bot, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, range);

    if (!targets.empty())
    {
        for (Unit* unit : targets)
        {
            bot->UpdatePosition(*unit);
            FleeManager manager(bot, sPlayerbotAIConfig.sightDistance, 0, true);
            float rx, ry, rz;
            if (manager.CalculateDestination(&rx, &ry, &rz))
            {
                WorldLocation loc(bot->GetMapId(), rx, ry, rz);
                locs.push_back(loc);
            }
        }
    }
    else
    {
        RandomTeleportForLevel(bot);
    }

    if (pmo)
        pmo->finish();

    Refresh(bot);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (bot->InBattleground())
        return;

    if (bot->GetLevel() < 3 || (bot->GetLevel() < 56 && bot->getClass() == CLASS_DEATH_KNIGHT))
    {
        RandomizeFirst(bot);
    }
    else if (bot->GetLevel() < sPlayerbotAIConfig.randomBotMaxLevel || !sPlayerbotAIConfig.downgradeMaxLevelBot)
    {
        uint8 level = bot->GetLevel();
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
        // IncreaseLevel(bot);
    }
    else
    {
        RandomizeFirst(bot);
    }
}

void RandomPlayerbotMgr::IncreaseLevel(Player* bot)
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "IncreaseLevel");
    uint32 lastLevel = GetValue(bot, "level");
    uint8 level = bot->GetLevel() + 1;
    if (level > maxLevel)
    {
        level = maxLevel;
    }
    if (lastLevel != level)
    {
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
    }

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // if lvl sync is enabled, max level is limited by online players lvl
    if (sPlayerbotAIConfig.syncLevelWithPlayers)
        maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel,
                            std::min(playersLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));

    uint32 minLevel = sPlayerbotAIConfig.randomBotMinLevel;
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        maxLevel = std::max(maxLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        minLevel = std::max(minLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    }

    if (uint32 cap = GetCommunityLevelCap())
        maxLevel = std::min(maxLevel, cap);

    if (maxLevel < minLevel)
        maxLevel = minLevel;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeFirst");

    uint32 level;

    if (sPlayerbotAIConfig.downgradeMaxLevelBot && bot->GetLevel() >= sPlayerbotAIConfig.randomBotMaxLevel)
    {
        if (bot->getClass() == CLASS_DEATH_KNIGHT)
        {
            level = sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);
        }
        else
        {
            level = sPlayerbotAIConfig.randomBotMinLevel;
        }
    }
    else
    {
        uint32 roll = urand(1, 100);
        if (roll <= 100 * sPlayerbotAIConfig.randomBotMaxLevelChance)
        {
            level = maxLevel;
        }
        else if (roll <=
                 (100 * (sPlayerbotAIConfig.randomBotMaxLevelChance + sPlayerbotAIConfig.randomBotMinLevelChance)))
        {
            level = minLevel;
        }
        else
        {
            level = urand(minLevel, maxLevel);
        }
    }

    if (sPlayerbotAIConfig.disableRandomLevels)
    {
        level = bot->getClass() == CLASS_DEATH_KNIGHT ? std::max(sPlayerbotAIConfig.randombotStartingLevel,
                                                                 sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                                                      : sPlayerbotAIConfig.randombotStartingLevel;
    }

    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();

    RandomTeleportForLevel(bot);
}

void RandomPlayerbotMgr::RandomizeMin(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeMin");
    uint32 level = sPlayerbotAIConfig.randomBotMinLevel;
    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::Clear(Player* bot)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.ClearEverything();
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float teleZ)
{
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    uint32 level = 0;
    QueryResult results = WorldDatabase.Query(
        "SELECT AVG(t.minlevel) minlevel, AVG(t.maxlevel) maxlevel FROM creature c "
        "INNER JOIN creature_template t ON c.id1 = t.entry WHERE map = {} AND minlevel > 1 AND ABS(position_x - {}) < "
        "{} AND ABS(position_y - {}) < {}",
        mapId, teleX, sPlayerbotAIConfig.randomBotTeleportDistance / 2, teleY,
        sPlayerbotAIConfig.randomBotTeleportDistance / 2);

    if (results)
    {
        Field* fields = results->Fetch();
        uint8 minLevel = fields[0].Get<uint8>();
        uint8 maxLevel = fields[1].Get<uint8>();
        level = urand(minLevel, maxLevel);
        if (level > maxLevel)
            level = maxLevel;
    }
    else
    {
        level = urand(1, maxLevel);
    }

    return level;
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (bot->isDead())
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        botAI->ResetStrategies(false);
    }

    // if (sPlayerbotAIConfig.disableRandomLevels)
    //     return;

    if (bot->InBattleground())
        return;

    LOG_DEBUG("playerbots", "Refreshing bot {} <{}>", bot->GetGUID().ToString().c_str(), bot->GetName().c_str());

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "Refresh");

    botAI->Reset();

    bot->DurabilityRepairAll(false, 1.0f, false);
    bot->SetFullHealth();
    bot->SetPvP(true);
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.Refresh();

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));

    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (GET_PLAYERBOT_AI(bot)->IsRealPlayer())
            return false;
    }
    if (bot)
    {
        return IsRandomBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsRandomBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);
    if (!sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(guid)))
        return false;

    if (std::find(currentBots.begin(), currentBots.end(), bot) != currentBots.end())
        return true;

    return false;
}

bool RandomPlayerbotMgr::IsAddclassBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (GET_PLAYERBOT_AI(bot)->IsRealPlayer())
            return false;
    }
    if (bot)
    {
        return IsAddclassBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsAddclassBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);

    // Check the cache with faction considerations
    for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
    {
        if (claz == 10)
            continue;

        for (uint8 isAlliance = 0; isAlliance <= 1; isAlliance++)
        {
            if (addclassCache[GetTeamClassIdx(isAlliance, claz)].find(guid) !=
                addclassCache[GetTeamClassIdx(isAlliance, claz)].end())
            {
                return true;
            }
        }
    }

    // If not in cache, check the account type
    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (accountId && IsAccountType(accountId, 2)) // Type 2 = AddClass
    {
        return true;
    }

    return false;
}

uint32 RandomPlayerbotMgr::GetTuningOrDefault(std::string const& key, uint32 def) const
{
    uint32 v = const_cast<RandomPlayerbotMgr*>(this)->GetEventValue(0, key);
    return v ? v : def;
}

void RandomPlayerbotMgr::GetBots()
{
    uint32 target = sPlayerbotAIConfig.rtgEventDriven ? GetEventValue(0, "rtg_target") : GetMaxAllowedBotCount();

    for (auto it = currentBots.begin(); it != currentBots.end();)
    {
        if (!GetEventValue(*it, "add"))
        {
            it = currentBots.erase(it);
            continue;
        }

        if (sPlayerbotAIConfig.rtgEventDriven)
        {
            std::string addData = GetEventData(*it, "add");
            if (!RTG::IsQueueManagedAddData(addData))
            {
                it = currentBots.erase(it);
                continue;
            }
        }

        ++it;
    }

    if (currentBots.size() >= target)
        return;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, "add");

    std::unordered_set<uint32> already(currentBots.begin(), currentBots.end());

    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();

            if (!GetEventValue(bot, "add"))
                continue;

            if (sPlayerbotAIConfig.rtgEventDriven)
            {
                std::string addData = GetEventData(bot, "add");
                if (!RTG::IsQueueManagedAddData(addData))
                    continue;
            }

            if (already.insert(bot).second)
                currentBots.push_back(bot);

            if (currentBots.size() >= target)
                break;

        } while (result->NextRow());
    }
}


std::vector<uint32> RandomPlayerbotMgr::GetBgBots(uint32 bracket)
{
    // if (!currentBgBots.empty()) return currentBgBots;

    std::vector<uint32> BgBots;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_EVENT_AND_VALUE);
    stmt->SetData(0, "bg");
    stmt->SetData(1, bracket);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            BgBots.push_back(bot);
        } while (result->NextRow());
    }

    return std::move(BgBots);
}

CachedEvent* RandomPlayerbotMgr::FindEvent(uint32 bot, std::string const& event)
{
    BotEventCache& cache = eventCache[bot];

    // Load once
    if (!cache.loaded)
    {
        cache.events.clear();

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_BOT);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);

        if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
        {
            do
            {
                Field* fields = result->Fetch();

                CachedEvent e;
                e.value = fields[1].Get<uint32>();
                e.lastChangeTime = fields[2].Get<uint32>();
                e.validIn = fields[3].Get<uint32>();
                e.data = fields[4].Get<std::string>();

                cache.events.emplace(fields[0].Get<std::string>(), std::move(e));
            } while (result->NextRow());
        }

        cache.loaded = true;
    }

    auto it = cache.events.find(event);
    if (it == cache.events.end())
        return nullptr;

    CachedEvent& e = it->second;

    // remove expired events
    if (e.validIn && (NowSeconds() - e.lastChangeTime) >= e.validIn && event != "specNo" && event != "specLink")
    {
        cache.events.erase(it);
        return nullptr;
    }

    return &e;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}

std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->data;

    return "";
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                                         std::string const& data)
{
    uint32 now = NowSeconds();
    if (RTG_QueueDebugEnabled() && RTG_IsQueueSupervisorEvent(event))
    {
        CachedEvent* oldEvent = FindEvent(bot, event);
        uint32 oldValue = oldEvent ? oldEvent->value : 0u;
        uint32 oldValidIn = oldEvent ? oldEvent->validIn : 0u;
        std::string oldData = oldEvent ? oldEvent->data : "";
        LOG_INFO("playerbots", "[RTGDBG][EVENT] bot={} event={} oldValue={} newValue={} oldTtl={} newTtl={} oldData='{}' newData='{}'",
                 bot, event, oldValue, value, oldValidIn, validIn, oldData, data);
    }

    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, bot);
    stmt->SetData(2, event.c_str());
    trans->Append(stmt);

    if (value)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_RANDOM_BOTS);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);
        stmt->SetData(2, now);
        stmt->SetData(3, validIn);
        stmt->SetData(4, event.c_str());
        stmt->SetData(5, value);

        if (!data.empty())
            stmt->SetData(6, data.c_str());
        else
            stmt->SetData(6);  // NULL

        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);

    // Update in-memory cache
    BotEventCache& cache = eventCache[bot];
    cache.loaded = true;

    if (!value)
    {
        cache.events.erase(event);
        return 0;
    }

    CachedEvent& e = cache.events[event];  // create-on-write is OK here
    e.value = value;
    e.lastChangeTime = now;
    e.validIn = validIn;
    e.data = data;

    return value;
}

uint32 RandomPlayerbotMgr::GetValue(uint32 bot, std::string const& type) { return GetEventValue(bot, type); }

uint32 RandomPlayerbotMgr::GetValue(Player* bot, std::string const& type)
{
    return GetValue(bot->GetGUID().GetCounter(), type);
}

std::string RandomPlayerbotMgr::GetData(uint32 bot, std::string const& type) { return GetEventData(bot, type); }

void RandomPlayerbotMgr::SetValue(uint32 bot, std::string const& type, uint32 value, std::string const& data)
{
    SetEventValue(bot, type, value, sPlayerbotAIConfig.maxRandomBotInWorldTime, data);
}

void RandomPlayerbotMgr::SetValue(Player* bot, std::string const& type, uint32 value, std::string const& data)
{
    SetValue(bot->GetGUID().GetCounter(), type, value, data);
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        LOG_ERROR("playerbots", "Playerbots system is currently disabled!");
        return false;
    }

    if (!args || !*args)
    {
        LOG_ERROR("playerbots", "Usage: rndbot stats/update/reset/init/refresh/add/remove");
        return false;
    }

    std::string const cmd = args;

    // Ratio tuning: .playerbots rndbot ratio [show|<value>]
if (cmd.rfind("ratio", 0) == 0)
{
    std::string arg;
    if (cmd.size() > 6)
        arg = cmd.substr(6);

    if (arg.empty() || arg == "show")
    {
        float dbRatio = sRandomPlayerbotMgr.LoadSavedBotsPerPlayerFromDB();

        handler->PSendSysMessage(
            "Ratio mode: {} | botsPerPlayer (DB) = {} | min={} max={}",
            sPlayerbotAIConfig.usePlayerCountRatio ? "ENABLED" : "DISABLED",
            dbRatio,
            sPlayerbotAIConfig.minRandomBots,
            sPlayerbotAIConfig.maxRandomBots);

        handler->PSendSysMessage("Usage: .playerbots rndbot ratio <value>  (example: 1.5)");
        return true;
    }

    float ratio = 0.0f;
    try { ratio = std::stof(arg); }
    catch (...)
    {
        handler->PSendSysMessage("Invalid ratio '{}'. Usage: .playerbots rndbot ratio <value>", arg);
        return false;
    }

    if (ratio <= 0.0f)
    {
        handler->PSendSysMessage("Ratio must be > 0. Usage: .playerbots rndbot ratio <value>");
        return false;
    }

    sRandomPlayerbotMgr.SaveBotsPerPlayerToDB(ratio);
    sRandomPlayerbotMgr.ForceBotCountRecheck();

    handler->PSendSysMessage("Randombot ratio set to {}. Population recalculation triggered.", ratio);
    LOG_INFO("playerbots", "[RatioCmd] botsPerPlayer={} – forced population recheck", ratio);
    return true;
}


    if (cmd == "reset")
    {
        PlayerbotsDatabase.Execute(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS));
        sRandomPlayerbotMgr.eventCache.clear();
        LOG_INFO("playerbots", "Random bots were reset for all players. Please restart the Server.");
        return true;
    }

    if (cmd == "stats")
    {
        sRandomPlayerbotMgr.PrintStats();
        // activatePrintStatsThread();
        return true;
    }

    if (cmd == "reload")
    {
        sPlayerbotAIConfig.Initialize();
        return true;
    }

    if (cmd == "update")
    {
        sRandomPlayerbotMgr.UpdateAIInternal(0);
        return true;
    }

    std::map<std::string, ConsoleCommandHandler> handlers;
    // handlers["initmin"] = &RandomPlayerbotMgr::RandomizeMin;
    handlers["init"] = &RandomPlayerbotMgr::RandomizeFirst;
    handlers["clear"] = &RandomPlayerbotMgr::Clear;
    handlers["levelup"] = handlers["level"] = &RandomPlayerbotMgr::IncreaseLevel;
    handlers["refresh"] = &RandomPlayerbotMgr::Refresh;
    handlers["teleport"] = &RandomPlayerbotMgr::RandomTeleportForLevel;
    // handlers["rpg"] = &RandomPlayerbotMgr::RandomTeleportForRpg;
    handlers["revive"] = &RandomPlayerbotMgr::Revive;
    handlers["grind"] = &RandomPlayerbotMgr::RandomTeleport;
    handlers["change_strategy"] = &RandomPlayerbotMgr::ChangeStrategy;

    for (std::map<std::string, ConsoleCommandHandler>::iterator j = handlers.begin(); j != handlers.end(); ++j)
    {
        std::string const prefix = j->first;
        if (cmd.find(prefix) != 0)
            continue;

        std::string const name = cmd.size() > prefix.size() + 1 ? cmd.substr(1 + prefix.size()) : "%";

        std::vector<uint32> botIds;
        for (std::vector<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin();
             i != sPlayerbotAIConfig.randomBotAccounts.end(); ++i)
        {
            uint32 account = *i;
            if (QueryResult results = CharacterDatabase.Query(
                    "SELECT guid FROM characters WHERE account = {} AND name like '{}'", account, name.c_str()))
            {
                do
                {
                    Field* fields = results->Fetch();

                    uint32 botId = fields[0].Get<uint32>();
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(botId);
                    if (!sRandomPlayerbotMgr.IsRandomBot(guid.GetCounter()))
                    {
                        continue;
                    }
                    Player* bot = ObjectAccessor::FindPlayer(guid);
                    if (!bot)
                        continue;

                    botIds.push_back(botId);
                } while (results->NextRow());
            }
        }

        if (botIds.empty())
        {
            LOG_INFO("playerbots", "Nothing to do");
            return false;
        }

        uint32 processed = 0;
        for (std::vector<uint32>::iterator i = botIds.begin(); i != botIds.end(); ++i)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(*i);
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot)
                continue;

            LOG_INFO("playerbots", "[{}/{}] Processing command {} for bot {}", processed++, botIds.size(), cmd.c_str(),
                     bot->GetName().c_str());

            ConsoleCommandHandler handler = j->second;
            (sRandomPlayerbotMgr.*handler)(bot);
        }

        return true;
    }

    // std::vector<std::string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args);
    // for (std::vector<std::string>::iterator i = messages.begin(); i != messages.end(); ++i)
    // {
    //     LOG_INFO("playerbots", "{}", i->c_str());
    // }
    return true;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, std::string const text, Player* fromPlayer, std::string channelName)
{
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot)
            continue;

        if (!channelName.empty())
        {
            if (ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId()))
            {
                Channel* chn = cMgr->GetChannel(channelName, bot);
                if (!chn)
                    continue;
            }
        }

        GET_PLAYERBOT_AI(bot)->HandleCommand(type, text, fromPlayer);
    }
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    if (player && IsRandomBot(player) && sPlayerbotAIConfig.rtgQueueOwnershipEnable)
        RTG::RtgQueueLedger::Instance().Remove(player->GetGUID().GetCounter());

    DisablePlayerBot(player->GetGUID());

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && player == botAI->GetMaster())
        {
            botAI->SetMaster(nullptr);
            if (!bot->InBattleground())
            {
                botAI->ResetStrategies();
            }
        }
    }

    std::vector<Player*>::iterator i = std::find(players.begin(), players.end(), player);
    if (i != players.end())
        players.erase(i);
}

void RandomPlayerbotMgr::OnBotLoginInternal(Player* const bot)
{
    if (_isBotLogging)
    {
        LOG_INFO("playerbots", "{}/{} Bot {} logged in", playerBots.size(),
                 sRandomPlayerbotMgr.GetMaxAllowedBotCount(), bot->GetName().c_str());

        if (playerBots.size() == sRandomPlayerbotMgr.GetMaxAllowedBotCount())
        {
            _isBotLogging = false;
        }
    }

    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        std::string addData = GetEventData(bot->GetGUID().GetCounter(), "add");
        uint32 desiredTeam = 0;
        uint32 desiredLevel = 0;
        uint32 desiredQueueType = 0;

        if (RTG::ParseLfgAddData(addData, desiredTeam, desiredLevel))
        {
            if (desiredLevel && bot->GetLevel() != desiredLevel)
            {
                bot->GiveLevel(desiredLevel);
                bot->InitStatsForLevel(true);
                bot->SetUInt32Value(PLAYER_XP, 0);
            }

            SetEventValue(bot->GetGUID().GetCounter(), "rtg_lfg_pending", 1, 45, addData);
            SetEventValue(bot->GetGUID().GetCounter(), "rtg_bg_pending", 0, 0);
        }
        else if (RTG::ParseBgAddData(addData, desiredTeam, desiredLevel, desiredQueueType))
        {
            if (desiredLevel && bot->GetLevel() != desiredLevel)
            {
                bot->GiveLevel(desiredLevel);
                bot->InitStatsForLevel(true);
                bot->SetUInt32Value(PLAYER_XP, 0);
            }

            SetEventValue(bot->GetGUID().GetCounter(), "rtg_bg_pending", 1, RTG_GetQueueGraceTtlSeconds(), addData);
            SetEventValue(bot->GetGUID().GetCounter(), "rtg_bg_queue_grace", 1, RTG_GetQueueGraceTtlSeconds(), addData);
            SetEventValue(bot->GetGUID().GetCounter(), "rtg_bg_queue_retry", 0, 0);
            SetEventValue(bot->GetGUID().GetCounter(), "rtg_lfg_pending", 0, 0);
            SetEventValue(bot->GetGUID().GetCounter(), "rtg_add_requested", 0, 0);

            if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
            {
                BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;
                uint32 minLevel = desiredLevel;
                uint32 maxLevel = desiredLevel;
                if (RTG_GetBgQueueContext(BattlegroundQueueTypeId(desiredQueueType), desiredLevel ? desiredLevel : bot->GetLevel(), bracketId, minLevel, maxLevel))
                {
                    RTG::RecordHelperReservation(bot, BattlegroundQueueTypeId(desiredQueueType), bracketId, TeamId(bot->GetTeamId()),
                                                 "event-driven battleground helper login", RTG::RtgHelperPurpose::StarterFill);
                    if (RTG_QueueOwnershipDebugEnabled())
                        LOG_INFO("playerbots", "[RTGDBG][OWNERSHIP] helper={} reserved queue={} bracket={} team={}", bot->GetGUID().GetCounter(), desiredQueueType, uint32(bracketId), bot->GetTeamId());
                }
            }

            RTG_RuntimeBreadcrumb(fmt::format("[RTG][LOGIN] helper={} queue={} team={} level={}",
                bot->GetGUID().GetCounter(), desiredQueueType, bot->GetTeamId(), bot->GetLevel()));
            RTG_DispatchImmediateBgQueueJoin(bot, desiredQueueType, "login_success");
        }
    }

    if (sPlayerbotAIConfig.randomBotFixedLevel)
    {
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }
    else
    {
        bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }
}


void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    uint32 botsNearby = 0;

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (player == bot /* || GET_PLAYERBOT_AI(player)*/)  // TEST
            continue;

        Cell playerCell(player->GetPositionX(), player->GetPositionY());
        Cell botCell(bot->GetPositionX(), bot->GetPositionY());

        // if (playerCell == botCell)
        // botsNearby++;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (botAI && member == player && (!botAI->GetMaster() || GET_PLAYERBOT_AI(botAI->GetMaster())))
            {
                if (!bot->InBattleground())
                {
                    botAI->SetMaster(player);
                    botAI->ResetStrategies();
                    botAI->TellMaster("Hello");
                }

                break;
            }
        }
    }

    if (botsNearby > 100 && false)
    {
        WorldPosition botPos(player);

        // botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);

        // player->TeleportTo(botPos);
        // player->Relocate(botPos.coord_x, botPos.coord_y, botPos.coord_z, botPos.orientation);

        if (!player->GetFactionTemplateEntry())
        {
            botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);
        }
        else
        {
            std::vector<TravelDestination*> dests = sTravelMgr.getRpgTravelDestinations(player, true, true, 200000.0f);

            do
            {
                RpgTravelDestination* dest = (RpgTravelDestination*)dests[urand(0, dests.size() - 1)];
                CreatureTemplate const* cInfo = dest->GetCreatureTemplate();
                if (!cInfo)
                    continue;

                FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->faction);
                ReputationRank reaction = Unit::GetFactionReactionTo(player->GetFactionTemplateEntry(), factionEntry);

                if (reaction > REP_NEUTRAL && dest->nearestPoint(&botPos)->m_mapId == player->GetMapId())
                {
                    botPos = *dest->nearestPoint(&botPos);
                    break;
                }
            } while (true);
        }

        player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        player->TeleportTo(botPos);

        // player->Relocate(botPos.getX(), botPos.getY(), botPos.getZ(), botPos.getO());
    }

    if (IsRandomBot(player))
    {
        // ObjectGuid::LowType guid = player->GetGUID().GetCounter(); //not used, conditional could be rewritten for
        // simplicity. line marked for removal.
    }
    else
    {
        players.push_back(player);
        LOG_DEBUG("playerbots", "Including non-random bot player {} into random bot update", player->GetName().c_str());
    }
}

void RandomPlayerbotMgr::OnPlayerLoginError(uint32 bot)
{
    std::string addData = GetEventData(bot, "add");
    SetEventValue(bot, "add", 0, 0);
    SetEventValue(bot, "rtg_bg_pending", 0, 0);
    SetEventValue(bot, "rtg_bg_queue_grace", 0, 0);
    SetEventValue(bot, "rtg_bg_queue_retry", 0, 0);
    SetEventValue(bot, "rtg_bg_retire_when_safe", 0, 0);
    SetEventValue(bot, "rtg_lfg_pending", 0, 0);
    currentBots.remove(bot);
    if (sPlayerbotAIConfig.rtgQueueOwnershipEnable)
        RTG::RtgQueueLedger::Instance().Remove(bot);
    RTG_RuntimeBreadcrumb(fmt::format("[RTG][LOGIN][FAIL] helper={} add='{}'", bot, addData));
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    if (players.empty())
        return nullptr;

    uint32 index = urand(0, players.size() - 1);
    return players[index];
}

void RandomPlayerbotMgr::PrintStats()
{
    printStatsTimer = time(nullptr);
    LOG_INFO("playerbots", "Random Bots Stats: {} online", playerBots.size());

    std::map<uint8, uint32> alliance, horde;
    for (uint32 i = 0; i < 10; ++i)
    {
        alliance[i] = 0;
        horde[i] = 0;
    }

    std::map<uint8, uint32> perRace;
    std::map<uint8, uint32> perClass;

    std::map<uint8, uint32> lvlPerRace;
    std::map<uint8, uint32> lvlPerClass;
    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        perRace[race] = 0;
        lvlPerRace[race] = 0;
    }

    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        perClass[cls] = 0;
        lvlPerClass[cls] = 0;
    }

    uint32 dps = 0;
    uint32 heal = 0;
    uint32 tank = 0;
    uint32 active = 0;
    uint32 update = 0;
    uint32 randomize = 0;
    uint32 teleport = 0;
    uint32 changeStrategy = 0;
    uint32 dead = 0;
    uint32 combat = 0;
    // uint32 revive = 0; //not used, line marked for removal.
    uint32 inFlight = 0;
    uint32 moving = 0;
    uint32 mounted = 0;
    uint32 inBg = 0;
    uint32 rest = 0;
    uint32 engine_noncombat = 0;
    uint32 engine_combat = 0;
    uint32 engine_dead = 0;
    std::unordered_map<NewRpgStatus, int> rpgStatusCount;
    // static NewRpgStatistic rpgStasticTotal;
    std::unordered_map<uint32, int> zoneCount;
    uint8 maxBotLevel = 0;
    for (PlayerBotMap::iterator i = playerBots.begin(); i != playerBots.end(); ++i)
    {
        Player* bot = i->second;
        if (IsAlliance(bot->getRace()))
            ++alliance[bot->GetLevel()];
        else
            ++horde[bot->GetLevel()];
        maxBotLevel = std::max(maxBotLevel, bot->GetLevel());

        ++perRace[bot->getRace()];
        ++perClass[bot->getClass()];

        lvlPerClass[bot->getClass()] += bot->GetLevel();
        lvlPerRace[bot->getRace()] += bot->GetLevel();

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_ERROR("playerbots", "Player/Bot {} is registered in sRandomPlayerbotMgr playerBots and has no bot AI!", bot->GetName().c_str());
            continue;
        }

        if (botAI->AllowActivity())
            ++active;

        if (botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Get())
            ++update;

        uint32 botId = bot->GetGUID().GetCounter();
        if (!GetEventValue(botId, "randomize"))
            ++randomize;

        if (!GetEventValue(botId, "teleport"))
            ++teleport;

        if (!GetEventValue(botId, "change_strategy"))
            ++changeStrategy;

        if (bot->isDead())
        {
            ++dead;
            // if (!GetEventValue(botId, "dead"))
            //++revive;
        }
        if (bot->IsInCombat())
        {
            ++combat;
        }
        if (bot->isMoving())
        {
            ++moving;
        }
        if (bot->IsInFlight())
        {
            ++inFlight;
        }
        if (bot->IsMounted())
        {
            ++mounted;
        }
        if (bot->InBattleground() || bot->InArena())
        {
            ++inBg;
        }
        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
        {
            ++rest;
        }
        if (botAI->GetState() == BOT_STATE_NON_COMBAT)
            ++engine_noncombat;
        else if (botAI->GetState() == BOT_STATE_COMBAT)
            ++engine_combat;
        else
            ++engine_dead;

        if (botAI->IsHeal(bot, true))
            ++heal;
        else if (botAI->IsTank(bot, true))
            ++tank;
        else
            ++dps;

        zoneCount[bot->GetZoneId()]++;

        if (sPlayerbotAIConfig.enableNewRpgStrategy)
        {
            rpgStatusCount[botAI->rpgInfo.status]++;
            rpgStasticTotal += botAI->rpgStatistic;
            botAI->rpgStatistic = NewRpgStatistic();
        }
    }

    LOG_INFO("playerbots", "Bots level:");
    // uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    uint32_t currentAlliance = 0, currentHorde = 0;
    uint32_t step = std::max(1, static_cast<int>((maxBotLevel + 4) / 8));
    uint32_t from = 1;

    for (uint8 i = 1; i <= maxBotLevel; ++i)
    {
        currentAlliance += alliance[i];
        currentHorde += horde[i];

        if (((i + 1) % step == 0) || i == maxBotLevel)
        {
            if (currentAlliance || currentHorde)
                LOG_INFO("playerbots", "    {}..{}: {} alliance, {} horde", from, i, currentAlliance, currentHorde);
            currentAlliance = 0;
            currentHorde = 0;
            from = i + 1;
        }
    }

    LOG_INFO("playerbots", "Bots race:");
    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        if (perRace[race])
        {
            uint32 lvl = lvlPerRace[race] * 10 / perRace[race];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatRace(race).c_str(), perRace[race],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots class:");
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (perClass[cls])
        {
            uint32 lvl = lvlPerClass[cls] * 10 / perClass[cls];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatClass(cls).c_str(), perClass[cls],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots role:");
    LOG_INFO("playerbots", "    tank: {}, heal: {}, dps: {}", tank, heal, dps);

    LOG_INFO("playerbots", "Bots status:");
    LOG_INFO("playerbots", "    Active: {}", active);
    LOG_INFO("playerbots", "    Moving: {}", moving);

    // LOG_INFO("playerbots", "Bots to:");
    // LOG_INFO("playerbots", "    update: {}", update);
    // LOG_INFO("playerbots", "    randomize: {}", randomize);
    // LOG_INFO("playerbots", "    teleport: {}", teleport);
    // LOG_INFO("playerbots", "    change_strategy: {}", changeStrategy);
    // LOG_INFO("playerbots", "    revive: {}", revive);

    LOG_INFO("playerbots", "    In flight: {}", inFlight);
    LOG_INFO("playerbots", "    On mount: {}", mounted);
    LOG_INFO("playerbots", "    In combat: {}", combat);
    LOG_INFO("playerbots", "    In BG: {}", inBg);
    LOG_INFO("playerbots", "    In Rest: {}", rest);
    LOG_INFO("playerbots", "    Dead: {}", dead);

    if (sPlayerbotAIConfig.enableNewRpgStrategy)
    {
        LOG_INFO("playerbots", "Bots rpg status:");
        LOG_INFO("playerbots",
                 "    Idle: {}, Rest: {}, GoGrind: {}, GoCamp: {}, MoveRandom: {}, MoveNpc: {}, DoQuest: {}, "
                 "TravelFlight: {}",
                 rpgStatusCount[RPG_IDLE], rpgStatusCount[RPG_REST], rpgStatusCount[RPG_GO_GRIND],
                 rpgStatusCount[RPG_GO_CAMP], rpgStatusCount[RPG_WANDER_RANDOM], rpgStatusCount[RPG_WANDER_NPC],
                 rpgStatusCount[RPG_DO_QUEST], rpgStatusCount[RPG_TRAVEL_FLIGHT]);

        LOG_INFO("playerbots", "Bots total quests:");
        LOG_INFO("playerbots", "    Accepted: {}, Rewarded: {}, Dropped: {}", rpgStasticTotal.questAccepted,
                 rpgStasticTotal.questRewarded, rpgStasticTotal.questDropped);
    }

    LOG_INFO("playerbots", "Bots engine:", dead);
    LOG_INFO("playerbots", "    Non-combat: {}, Combat: {}, Dead: {}", engine_noncombat, engine_combat, engine_dead);
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(50, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

void RandomPlayerbotMgr::AddTradeDiscount(Player* bot, Player* master, int32 value)
{
    if (!master)
        return;

    uint32 discount = GetTradeDiscount(bot, master);
    int32 result = (int32)discount + value;
    discount = (result < 0 ? 0 : result);

    SetTradeDiscount(bot, master, discount);
}

void RandomPlayerbotMgr::SetTradeDiscount(Player* bot, Player* master, uint32 value)
{
    if (!master)
        return;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    SetEventValue(botId, name.str(), value, sPlayerbotAIConfig.maxRandomBotInWorldTime);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot, Player* master)
{
    if (!master)
        return 0;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    return GetEventValue(botId, name.str());
}

std::string const RandomPlayerbotMgr::HandleRemoteCommand(std::string const request)
{
    std::string::const_iterator pos = std::find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        std::ostringstream out;
        out << "invalid request: " << request;
        return out.str();
    }

    std::string const command = std::string(request.begin(), pos);
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(atoi(std::string(pos + 1, request.end()).c_str()));
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return "invalid guid";

    return botAI->HandleRemoteCommand(command);
}

void RandomPlayerbotMgr::ChangeStrategy(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        ScheduleTeleport(bot, 30);
    }
    else
    {
        LOG_INFO("playerbots", "Changing strategy for bot #{} <{}> to RPG", bot, player->GetName().c_str());
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        SetEventValue(bot, "teleport", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
    }

    ScheduleChangeStrategy(bot);
}

void RandomPlayerbotMgr::ChangeStrategyOnce(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)  // select grind / pvp
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        Refresh(player);
    }
    else
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
    }
}

void RandomPlayerbotMgr::RandomTeleportForRpg(Player* bot)
{
    uint32 race = bot->getRace();
    uint32 level = bot->GetLevel();
    LOG_DEBUG("playerbots", "Random teleporting bot {} for RPG ({} locations available)", bot->GetName().c_str(),
              rpgLocsCacheLevel[race].size());
    RandomTeleport(bot, rpgLocsCacheLevel[race][level], true);
}

void RandomPlayerbotMgr::Remove(Player* bot)
{
    ObjectGuid owner = bot->GetGUID();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER);
    stmt->SetData(0, 0);
    stmt->SetData(1, owner.GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    uint32 botId = owner.GetCounter();
    eventCache.erase(botId);

    LogoutPlayerBot(owner);
}

CreatureData const* RandomPlayerbotMgr::GetCreatureDataByEntry(uint32 entry)
{
    if (entry != 0)
    {
        for (auto const& itr : sObjectMgr->GetAllCreatureData())
            if (itr.second.id1 == entry)
                return &itr.second;
    }

    return nullptr;
}

ObjectGuid RandomPlayerbotMgr::GetBattleMasterGUID(Player* bot, BattlegroundTypeId bgTypeId)
{
    ObjectGuid battleMasterGUID = ObjectGuid::Empty;

    TeamId team = bot->GetTeamId();
    std::vector<uint32> Bms;

    for (auto i = std::begin(BattleMastersCache[team][bgTypeId]); i != std::end(BattleMastersCache[team][bgTypeId]);
         ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    for (auto i = std::begin(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]);
         i != std::end(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    if (Bms.empty())
        return battleMasterGUID;

    float dist1 = FLT_MAX;

    for (auto i = begin(Bms); i != end(Bms); ++i)
    {
        CreatureData const* data = sRandomPlayerbotMgr.GetCreatureDataByEntry(*i);
        if (!data)
            continue;

        Unit* Bm = PlayerbotAI::GetUnit(data);
        if (!Bm)
            continue;

        if (bot->GetMapId() != Bm->GetMapId())
            continue;

        // return first available guid on map if queue from anywhere
        if (!BattlegroundMgr::IsArenaType(bgTypeId))
        {
            battleMasterGUID = Bm->GetGUID();
            break;
        }

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(Bm->GetZoneId());
        if (!zone)
            continue;

        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (Bm->getDeathState() == DeathState::Dead)
            continue;

        float dist2 = sServerFacade.GetDistance2d(bot, data->posX, data->posY);
        if (dist2 < dist1)
        {
            dist1 = dist2;
            battleMasterGUID = Bm->GetGUID();
        }
    }

    return battleMasterGUID;
}
