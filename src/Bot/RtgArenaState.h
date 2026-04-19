#ifndef _PLAYERBOT_RTGARENASTATE_H
#define _PLAYERBOT_RTGARENASTATE_H

#include "ObjectGuid.h"

#include <string>

class RandomPlayerbotMgr;

namespace RTG
{
enum class ArenaHelperState : uint32
{
    None = 0,
    Queued = 1,
    Invited = 2,
    InInstance = 3,
    LeavingInstance = 4,
    ReturnedWorld = 5,
    RetirePending = 6,
    Retired = 7
};

char const* ArenaHelperStateName(ArenaHelperState state);

ArenaHelperState GetArenaHelperState(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
void SetArenaHelperState(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, ArenaHelperState state, uint32 ttlSeconds,
    std::string const& addData, char const* reason = nullptr);

uint32 GetArenaHelperCycle(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
uint32 AdvanceArenaHelperCycle(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, std::string const& addData, char const* reason);

uint32 GetArenaHelperInstanceId(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
void SetArenaHelperInstanceId(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 instanceId, uint32 ttlSeconds,
    std::string const& addData);

uint32 GetArenaMatchBirth(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
void SetArenaMatchBirth(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData);

uint32 GetArenaLastSeenLive(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
void SetArenaLastSeenLive(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData);

uint32 GetArenaLastSeenWorld(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId);
void SetArenaLastSeenWorld(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, uint32 value, uint32 ttlSeconds,
    std::string const& addData);
}

#endif
