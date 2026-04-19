#include "RtgArenaState.h"

#include "RandomPlayerbotMgr.h"

namespace RTG
{
namespace
{
char const* const kArenaStateKey = "rtg_arena_state";
char const* const kArenaCycleKey = "rtg_arena_cycle";
char const* const kArenaInstanceIdKey = "rtg_arena_instance_id";
char const* const kArenaMatchBirthKey = "rtg_arena_match_birth";
char const* const kArenaLastSeenLiveKey = "rtg_arena_last_seen_live";
char const* const kArenaLastSeenWorldKey = "rtg_arena_last_seen_world";
char const* const kArenaTeardownQuarantineKey = "rtg_arena_teardown_quarantine";
}

char const* ArenaHelperStateName(ArenaHelperState state)
{
    switch (state)
    {
        case ArenaHelperState::Queued: return "queued";
        case ArenaHelperState::Invited: return "invited";
        case ArenaHelperState::InInstance: return "in_instance";
        case ArenaHelperState::LeavingInstance: return "leaving_instance";
        case ArenaHelperState::ReturnedWorld: return "returned_world";
        case ArenaHelperState::RetirePending: return "retire_pending";
        case ArenaHelperState::Retired: return "retired";
        default: return "none";
    }
}

ArenaHelperState GetArenaHelperState(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return ArenaHelperState(mgr.RTG_GetBotEventValue(botId, kArenaStateKey));
}

void SetArenaHelperState(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, ArenaHelperState state, uint32 ttlSeconds,
    std::string const& addData, char const* reason)
{
    ArenaHelperState oldState = GetArenaHelperState(mgr, botId);
    if (oldState == state && !reason)
        return;

    mgr.RTG_SetBotEventValue(botId, kArenaStateKey, uint32(state), ttlSeconds, addData);
    if (oldState != state)
    {
        LOG_INFO("playerbots", "[RTG][ARENA][STATE] helper={} state={} prev={} cycle={} instance={} reason={}",
            botId, ArenaHelperStateName(state), ArenaHelperStateName(oldState), GetArenaHelperCycle(mgr, botId),
            GetArenaHelperInstanceId(mgr, botId), reason ? reason : "update");
    }
}

uint32 GetArenaHelperCycle(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaCycleKey);
}

uint32 AdvanceArenaHelperCycle(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, std::string const& addData, char const* reason)
{
    uint32 nextCycle = GetArenaHelperCycle(mgr, botId) + 1u;
    mgr.RTG_SetBotEventValue(botId, kArenaCycleKey, nextCycle, 7200u, addData);
    LOG_INFO("playerbots", "[RTG][ARENA][CYCLE] helper={} cycle={} reason={}",
        botId, nextCycle, reason ? reason : "advance");
    return nextCycle;
}

uint32 GetArenaHelperInstanceId(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaInstanceIdKey);
}

void SetArenaHelperInstanceId(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 instanceId, uint32 ttlSeconds,
    std::string const& addData)
{
    mgr.RTG_SetBotEventValue(botId, kArenaInstanceIdKey, instanceId, ttlSeconds, addData);
}

uint32 GetArenaMatchBirth(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaMatchBirthKey);
}

void SetArenaMatchBirth(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData)
{
    mgr.RTG_SetBotEventValue(botId, kArenaMatchBirthKey, value, ttlSeconds, addData);
}

uint32 GetArenaLastSeenLive(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaLastSeenLiveKey);
}

void SetArenaLastSeenLive(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData)
{
    mgr.RTG_SetBotEventValue(botId, kArenaLastSeenLiveKey, value, ttlSeconds, addData);
}

uint32 GetArenaLastSeenWorld(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaLastSeenWorldKey);
}

void SetArenaLastSeenWorld(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData)
{
    mgr.RTG_SetBotEventValue(botId, kArenaLastSeenWorldKey, value, ttlSeconds, addData);
}

bool IsArenaTeardownQuarantined(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId)
{
    return mgr.RTG_GetBotEventValue(botId, kArenaTeardownQuarantineKey) != 0u;
}

void SetArenaTeardownQuarantine(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, bool enabled, uint32 ttlSeconds,
    std::string const& addData, char const* reason)
{
    bool oldValue = IsArenaTeardownQuarantined(mgr, botId);
    if (oldValue == enabled && !reason)
        return;

    mgr.RTG_SetBotEventValue(botId, kArenaTeardownQuarantineKey, enabled ? 1u : 0u, enabled ? ttlSeconds : 0u, addData);
    if (oldValue != enabled)
    {
        LOG_INFO("playerbots", "[RTG][ARENA][TEARDOWN_QUARANTINE] helper={} enabled={} state={} cycle={} instance={} reason={}",
            botId, enabled ? 1u : 0u, ArenaHelperStateName(GetArenaHelperState(mgr, botId)), GetArenaHelperCycle(mgr, botId),
            GetArenaHelperInstanceId(mgr, botId), reason ? reason : "update");
    }
}
}
