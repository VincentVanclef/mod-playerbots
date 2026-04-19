#include "RtgArenaLifecycle.h"

#include "Battleground.h"
#include "Map.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <vector>

namespace RTG
{
namespace
{
struct ArenaInstanceRecord
{
    uint32 queueType = 0;
    uint32 bracketId = 0;
    bool isRated = false;
    uint32 instanceId = 0;
    uint32 status = 0;
    uint32 realPlayers = 0;
    uint32 helpers = 0;
    uint32 lastSeenAt = 0;
};

std::unordered_map<uint64, ArenaInstanceRecord> sArenaObservedInstances;
std::unordered_map<uint64, uint32> sArenaPreviousCounts;

uint64 MakeArenaInstanceKey(uint32 queueType, uint32 bracketId, bool isRated, uint32 instanceId)
{
    return (uint64(queueType) << 32) | (uint64(bracketId & 0x7FFFu) << 16) | (uint64(isRated ? 1u : 0u) << 15) | uint64(instanceId & 0x7FFFu);
}

uint64 MakeArenaCountKey(uint32 queueType, uint32 bracketId, bool isRated)
{
    return (uint64(queueType) << 32) | (uint64(bracketId) << 1) | uint64(isRated ? 1u : 0u);
}

char const* DetermineArenaBlockStage(Player* bot, BattlegroundQueueTypeId queueTypeId, bool desiredPresence,
    bool activeDesiredPresence, bool lifecycleOwned, bool instanceHasRealPlayers)
{
    if (!bot)
        return "bot_missing";
    if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        return "still_in_queue";
    if (bot->IsInvitedForBattlegroundInstance())
        return "still_invited";
    if (bot->InArena() || bot->InBattleground())
        return "still_in_instance";
    if (desiredPresence)
        return "desired_presence_stale";
    if (activeDesiredPresence)
        return "active_presence_stale";
    if (bot->GetBattleground())
        return instanceHasRealPlayers ? "bg_pointer_live" : "stale_instance_count";
    if (Map* map = bot->GetMap())
    {
        if (map->IsBattlegroundOrArena())
            return "map_residue";
    }
    if (lifecycleOwned)
        return "lifecycle_owned_stale";
    return nullptr;
}

void LogArenaTeardown(Player* bot, uint32 queueType, char const* tag, char const* stage, ArenaTeardownResult const& result,
    bool desiredPresence, bool activeDesiredPresence, bool lifecycleOwned, bool worldReturnPending, bool retireWhenSafe,
    bool leaveRequested)
{
    if (!bot)
        return;

    static std::unordered_map<uint64, uint32> sNextLogAt;
    uint32 nowTs = static_cast<uint32>(time(nullptr));
    uint64 logKey = ((uint64(bot->GetGUID().GetCounter()) << 32) | uint64(queueType)) ^
        (tag && std::string(tag) == "TEARDOWN_BLOCK" ? 0x1ull : 0ull);
    uint32& nextLogAt = sNextLogAt[logKey];
    if (nextLogAt && nowTs < nextLogAt)
        return;
    nextLogAt = nowTs + 10u;

    Battleground* bg = bot->GetBattleground();
    Map* map = bot->GetMap();
    LOG_INFO("playerbots",
        "[RTG][ARENA][{}] helper={} queue={} stage={} state={} cycle={} instance={} inQueue={} invited={} inArena={} inBattleground={} desiredPresence={} activeDesiredPresence={} lifecycleOwned={} instanceHasRealPlayers={} map={} bgStatus={} worldReturnPending={} retireWhenSafe={} leaveRequested={} staleSince={}",
        tag ? tag : "TEARDOWN", bot->GetGUID().GetCounter(), queueType, stage ? stage : "state",
        ArenaHelperStateName(result.helperState), result.cycleId, result.instanceId,
        bot->InBattlegroundQueue() ? 1 : 0,
        bot->IsInvitedForBattlegroundInstance() ? 1 : 0,
        bot->InArena() ? 1 : 0,
        bot->InBattleground() ? 1 : 0,
        desiredPresence ? 1 : 0,
        activeDesiredPresence ? 1 : 0,
        lifecycleOwned ? 1 : 0,
        result.instanceHasRealPlayers ? 1 : 0,
        map ? map->GetId() : 0u,
        bg ? uint32(bg->GetStatus()) : 0u,
        worldReturnPending ? 1 : 0,
        retireWhenSafe ? 1 : 0,
        leaveRequested ? 1 : 0,
        result.staleSince);
}
}

void BeginArenaTrackingCycle()
{
    sArenaObservedInstances.clear();
}

void ObserveArenaQueueParticipant(RandomPlayerbotMgr& mgr, Player* player, BattlegroundQueueTypeId queueTypeId,
    BattlegroundBracketId bracketId, bool isRated, bool isBot)
{
    if (!player)
        return;

    Battleground* bg = player->GetBattleground();
    if (!(player->InArena() && bg))
        return;

    uint32 instanceId = bg->GetInstanceID();
    if (!instanceId)
        return;

    uint32 nowTs = static_cast<uint32>(time(nullptr));
    ArenaInstanceRecord& record = sArenaObservedInstances[MakeArenaInstanceKey(uint32(queueTypeId), uint32(bracketId), isRated, instanceId)];
    record.queueType = uint32(queueTypeId);
    record.bracketId = uint32(bracketId);
    record.isRated = isRated;
    record.instanceId = instanceId;
    record.status = uint32(bg->GetStatus());
    record.lastSeenAt = nowTs;
    if (isBot)
        ++record.helpers;
    else
        ++record.realPlayers;

    if (!isBot)
        return;

    ObjectGuid::LowType botId = player->GetGUID().GetCounter();
    std::string addData = mgr.RTG_GetBotEventData(botId, "add");
    ArenaHelperState currentState = GetArenaHelperState(mgr, botId);
    uint32 storedInstanceId = GetArenaHelperInstanceId(mgr, botId);
    if (!GetArenaHelperCycle(mgr, botId) || currentState == ArenaHelperState::ReturnedWorld || currentState == ArenaHelperState::Retired)
        AdvanceArenaHelperCycle(mgr, botId, addData, "instance_entry");

    if (storedInstanceId != instanceId)
    {
        SetArenaHelperInstanceId(mgr, botId, instanceId, 7200u, addData);
        SetArenaMatchBirth(mgr, botId, nowTs, 7200u, addData);
        LOG_INFO("playerbots", "[RTG][ARENA][INSTANCE_ASSIGN] helper={} cycle={} instance={} queue={} bracket={}",
            botId, GetArenaHelperCycle(mgr, botId), instanceId, uint32(queueTypeId), uint32(bracketId));
    }

    SetArenaLastSeenLive(mgr, botId, nowTs, 7200u, addData);
    SetArenaLastSeenWorld(mgr, botId, 0u, 0u, addData);
    SetArenaHelperState(mgr, botId, ArenaHelperState::InInstance, 7200u, addData, "live_instance");
}

void FinalizeArenaTrackingCycle(RandomPlayerbotMgr& mgr, std::map<uint32, std::map<uint32, BattlegroundInfo>>& battlegroundData)
{
    (void)mgr;
    for (auto& queuePair : battlegroundData)
    {
        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queuePair.first);
        if (BattlegroundMgr::BGArenaType(queueTypeId) == ARENA_TYPE_NONE && uint32(queueTypeId) != 9u)
            continue;

        for (auto& bracketPair : queuePair.second)
        {
            BattlegroundInfo& info = bracketPair.second;
            uint32 bracketId = bracketPair.first;

            for (bool isRated : {false, true})
            {
                std::vector<uint32> authoritativeInstances;
                for (auto const& observedPair : sArenaObservedInstances)
                {
                    ArenaInstanceRecord const& record = observedPair.second;
                    if (record.queueType != uint32(queueTypeId) || record.bracketId != bracketId || record.isRated != isRated)
                        continue;
                    if (record.status == STATUS_WAIT_LEAVE)
                        continue;
                    if (record.realPlayers == 0u)
                        continue;
                    authoritativeInstances.push_back(record.instanceId);
                }

                std::sort(authoritativeInstances.begin(), authoritativeInstances.end());
                authoritativeInstances.erase(std::unique(authoritativeInstances.begin(), authoritativeInstances.end()), authoritativeInstances.end());

                std::vector<uint32>& targetInstances = isRated ? info.ratedArenaInstances : info.skirmishArenaInstances;
                uint32& targetCount = isRated ? info.ratedArenaInstanceCount : info.skirmishArenaInstanceCount;
                uint32 rawCount = targetCount;
                targetInstances = authoritativeInstances;
                targetCount = static_cast<uint32>(authoritativeInstances.size());

                uint64 countKey = MakeArenaCountKey(uint32(queueTypeId), bracketId, isRated);
                uint32 previousCount = sArenaPreviousCounts[countKey];
                if (previousCount != targetCount || rawCount != targetCount)
                {
                    char const* reason = rawCount > targetCount ? "decrement_instance_dead" :
                        (rawCount < targetCount ? "increment_instance_live" : "refresh_real_occupancy");
                    LOG_INFO("playerbots", "[RTG][ARENA][INSTANCE_COUNT] queue={} bracket={} rated={} activeInstances={} raw={} reason={}",
                        uint32(queueTypeId), bracketId, isRated ? 1u : 0u, targetCount, rawCount, reason);
                    sArenaPreviousCounts[countKey] = targetCount;
                }
            }
        }
    }
}

bool ArenaInstanceHasRealPlayers(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, bool isRated, uint32 instanceId)
{
    if (!instanceId)
        return false;

    auto itr = sArenaObservedInstances.find(MakeArenaInstanceKey(uint32(queueTypeId), uint32(bracketId), isRated, instanceId));
    if (itr == sArenaObservedInstances.end())
        return false;

    return itr->second.realPlayers > 0u && itr->second.status != STATUS_WAIT_LEAVE;
}

ArenaTeardownResult ObserveArenaTeardown(RandomPlayerbotMgr& mgr, Player* bot, uint32 desiredQueueType, BattlegroundBracketId bracketId,
    uint32 plannerPhase, bool hasRealDemand, uint32 plannerTeamNeed, bool desiredPresence, bool activeDesiredPresence,
    bool lifecycleOwned, bool worldReturnPending, bool retireWhenSafe, bool leaveRequested, std::string const& addData,
    uint32 staleTimeoutSeconds)
{
    ArenaTeardownResult result;
    if (!bot || desiredQueueType <= BATTLEGROUND_QUEUE_NONE || desiredQueueType >= MAX_BATTLEGROUND_QUEUE_TYPES)
        return result;

    ObjectGuid::LowType botId = bot->GetGUID().GetCounter();
    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(desiredQueueType);
    uint32 nowTs = static_cast<uint32>(time(nullptr));
    Battleground* bg = bot->GetBattleground();
    uint32 instanceId = bg ? bg->GetInstanceID() : GetArenaHelperInstanceId(mgr, botId);
    bool isRated = (bg && bg->isRated());
    bool inQueue = bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
    bool invited = bot->IsInvitedForBattlegroundInstance();
    bool inInstance = (bot->InArena() || bot->InBattleground()) && bot->GetBattlegroundTypeId() == BattlegroundMgr::BGTemplateId(queueTypeId);
    bool pointerResidue = bg && !inInstance;
    bool mapResidue = bot->GetMap() && bot->GetMap()->IsBattlegroundOrArena() && !bot->InArena() && !bot->InBattleground();

    result.dormantNoDemand = plannerPhase == 0u && !hasRealDemand && plannerTeamNeed == 0u;
    result.instanceId = instanceId;
    result.cycleId = GetArenaHelperCycle(mgr, botId);
    result.instanceHasRealPlayers = ArenaInstanceHasRealPlayers(queueTypeId, bracketId, isRated, instanceId);
    result.quarantineActive = IsArenaTeardownQuarantined(mgr, botId);
    result.closureState = GetArenaClosureState(mgr, botId);

    ArenaHelperState currentState = GetArenaHelperState(mgr, botId);
    if ((inQueue || invited || inInstance) &&
        (currentState == ArenaHelperState::None || currentState == ArenaHelperState::ReturnedWorld || currentState == ArenaHelperState::Retired))
        result.cycleId = AdvanceArenaHelperCycle(mgr, botId, addData, "arena_reentry");

    if (inInstance)
    {
        if (GetArenaHelperInstanceId(mgr, botId) != instanceId)
        {
            SetArenaHelperInstanceId(mgr, botId, instanceId, 7200u, addData);
            SetArenaMatchBirth(mgr, botId, nowTs, 7200u, addData);
            LOG_INFO("playerbots", "[RTG][ARENA][INSTANCE_ASSIGN] helper={} cycle={} instance={} queue={} bracket={}",
                botId, GetArenaHelperCycle(mgr, botId), instanceId, desiredQueueType, uint32(bracketId));
        }
        SetArenaLastSeenLive(mgr, botId, nowTs, 7200u, addData);
        SetArenaLastSeenWorld(mgr, botId, 0u, 0u, addData);
        SetArenaHelperState(mgr, botId, ArenaHelperState::InInstance, 7200u, addData, "in_instance");
        result.helperState = ArenaHelperState::InInstance;
    }
    else if (invited)
    {
        SetArenaHelperState(mgr, botId, ArenaHelperState::Invited, 7200u, addData, "invited");
        result.helperState = ArenaHelperState::Invited;
    }
    else if (inQueue)
    {
        SetArenaHelperState(mgr, botId, ArenaHelperState::Queued, 7200u, addData, "queued");
        result.helperState = ArenaHelperState::Queued;
    }
    else if (leaveRequested || worldReturnPending)
    {
        SetArenaHelperState(mgr, botId, ArenaHelperState::LeavingInstance, 7200u, addData, "leaving");
        result.helperState = ArenaHelperState::LeavingInstance;
    }
    else if (GetArenaHelperInstanceId(mgr, botId) != 0u)
    {
        SetArenaLastSeenWorld(mgr, botId, nowTs, 7200u, addData);
        SetArenaHelperState(mgr, botId, ArenaHelperState::ReturnedWorld, 7200u, addData, "returned_world");
        LOG_INFO("playerbots", "[RTG][ARENA][INSTANCE_END] helper={} cycle={} instance={} reason=returned_world",
            botId, GetArenaHelperCycle(mgr, botId), GetArenaHelperInstanceId(mgr, botId));
        SetArenaHelperInstanceId(mgr, botId, 0u, 0u, addData);
        SetArenaMatchBirth(mgr, botId, 0u, 0u, addData);
        result.helperState = ArenaHelperState::ReturnedWorld;
        result.instanceId = 0u;
    }
    else
        result.helperState = GetArenaHelperState(mgr, botId);

    result.cycleId = GetArenaHelperCycle(mgr, botId);
    result.instanceId = GetArenaHelperInstanceId(mgr, botId);
    result.staleSince = mgr.RTG_GetBotEventValue(botId, "rtg_arena_stale_instance_since");

    if ((retireWhenSafe || leaveRequested || worldReturnPending) && !result.quarantineActive)
    {
        if (result.closureState == ArenaClosureState::None)
        {
            SetArenaClosureState(mgr, botId, ArenaClosureState::Pending, 1800u, addData, "match_end");
            LOG_INFO("playerbots", "[RTG][ARENA][CLOSURE_BEGIN] helper={} cycle={} instance={} reason=match_end",
                botId, result.cycleId, result.instanceId);
            result.closureState = ArenaClosureState::Pending;
        }
        SetArenaTeardownQuarantine(mgr, botId, true, 300u, addData, "teardown_start");
        result.quarantineActive = true;
    }

    result.residualAttachment = result.dormantNoDemand &&
        (lifecycleOwned || desiredPresence || activeDesiredPresence || inQueue || invited || inInstance ||
         pointerResidue || mapResidue || worldReturnPending || retireWhenSafe || leaveRequested);
    result.staleDetachEligible = result.dormantNoDemand &&
        !desiredPresence && !activeDesiredPresence && !inQueue && !invited && !inInstance &&
        (lifecycleOwned || pointerResidue || mapResidue || (result.instanceId != 0u && !result.instanceHasRealPlayers));
    result.blockStage = DetermineArenaBlockStage(bot, queueTypeId, desiredPresence, activeDesiredPresence, lifecycleOwned, result.instanceHasRealPlayers);

    if (result.residualAttachment)
        LogArenaTeardown(bot, desiredQueueType, "TEARDOWN", "state", result, desiredPresence, activeDesiredPresence,
            lifecycleOwned, worldReturnPending, retireWhenSafe, leaveRequested);
    if (result.dormantNoDemand && !worldReturnPending && result.blockStage)
    {
        LogArenaTeardown(bot, desiredQueueType, "TEARDOWN_BLOCK", result.blockStage, result, desiredPresence,
            activeDesiredPresence, lifecycleOwned, worldReturnPending, retireWhenSafe, leaveRequested);
        if (result.closureState == ArenaClosureState::Pending || result.quarantineActive)
        {
            LOG_INFO("playerbots",
                "[RTG][ARENA][CLOSURE_BLOCK] helper={} cycle={} instance={} stage={} closureState={} quarantine={} instanceHasRealPlayers={}",
                botId, result.cycleId, result.instanceId, result.blockStage,
                ArenaClosureStateName(result.closureState), result.quarantineActive ? 1u : 0u,
                result.instanceHasRealPlayers ? 1u : 0u);
        }
    }

    if (result.quarantineActive && result.instanceHasRealPlayers)
    {
        LOG_INFO("playerbots", "[RTG][ARENA][PLAYER_LINGER] helper={} cycle={} instance={} playerStillInArena=1 quarantine=1 state={}",
            botId, result.cycleId, result.instanceId, ArenaHelperStateName(result.helperState));
    }

    if (result.quarantineActive && (inQueue || invited) && !inInstance)
    {
        LOG_INFO("playerbots", "[RTG][ARENA][QUEUE_LEAK] helper={} cycle={} instance={} inQueue={} invited={} quarantine=1 state={}",
            botId, result.cycleId, result.instanceId, inQueue ? 1u : 0u, invited ? 1u : 0u,
            ArenaHelperStateName(result.helperState));
    }

    if (result.staleDetachEligible)
    {
        if (!result.staleSince)
        {
            result.staleSince = nowTs;
            mgr.RTG_SetBotEventValue(botId, "rtg_arena_stale_instance_since", result.staleSince, 120u, addData);
        }
        else
            mgr.RTG_SetBotEventValue(botId, "rtg_arena_stale_instance_since", result.staleSince, 120u, addData);

        result.forceTeardown = nowTs > result.staleSince && (nowTs - result.staleSince) >= staleTimeoutSeconds;
        if (result.forceTeardown)
        {
            SetArenaClosureState(mgr, botId, ArenaClosureState::Pending, 1800u, addData, "stale_instance_timeout");
            SetArenaHelperState(mgr, botId, ArenaHelperState::RetirePending, 120u, addData, "stale_instance_timeout");
            LOG_INFO("playerbots", "[RTG][ARENA][TEARDOWN_FORCE] helper={} cycle={} instance={} reason=stale_instance_timeout staleFor={} instanceHasRealPlayers={}",
                botId, result.cycleId, result.instanceId, nowTs - result.staleSince, result.instanceHasRealPlayers ? 1u : 0u);
            result.closureState = ArenaClosureState::Pending;
        }
    }
    else if (result.quarantineActive && !result.instanceHasRealPlayers && !desiredPresence && !activeDesiredPresence && !inInstance)
    {
        result.forceTeardown = true;
        SetArenaClosureState(mgr, botId, ArenaClosureState::Pending, 1800u, addData, "player_exit_cleanup");
        SetArenaHelperState(mgr, botId, ArenaHelperState::RetirePending, 120u, addData, "player_exit_cleanup");
        LOG_INFO("playerbots", "[RTG][ARENA][TEARDOWN_FORCE] helper={} cycle={} instance={} reason=player_exit_cleanup quarantine=1",
            botId, result.cycleId, result.instanceId);
        result.closureState = ArenaClosureState::Pending;
    }
    else if (result.staleSince)
        mgr.RTG_SetBotEventValue(botId, "rtg_arena_stale_instance_since", 0u, 0u);

    return result;
}

void MarkArenaHelperRetired(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, std::string const& addData, char const* reason)
{
    SetArenaClosureState(mgr, botId, ArenaClosureState::Complete, 1800u, addData, reason ? reason : "retired");
    LOG_INFO("playerbots", "[RTG][ARENA][CLOSURE_PURGE] helper={} reason={}", botId, reason ? reason : "retired");
    SetArenaHelperState(mgr, botId, ArenaHelperState::Retired, 120u, addData, reason ? reason : "retired");
    mgr.RTG_SetBotEventValue(botId, "rtg_arena_stale_instance_since", 0u, 0u, addData);
    SetArenaTeardownQuarantine(mgr, botId, false, 0u, addData, reason ? reason : "retired");
    SetArenaHelperInstanceId(mgr, botId, 0u, 0u, addData);
    SetArenaMatchBirth(mgr, botId, 0u, 0u, addData);
    SetArenaLastSeenLive(mgr, botId, 0u, 0u, addData);
}
}
