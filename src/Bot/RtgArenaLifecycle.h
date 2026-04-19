#ifndef _PLAYERBOT_RTGARENALIFECYCLE_H
#define _PLAYERBOT_RTGARENALIFECYCLE_H

#include "BattlegroundMgr.h"
#include "RtgArenaState.h"

#include <map>
#include <string>

class Player;
class RandomPlayerbotMgr;
struct BattlegroundInfo;

namespace RTG
{
struct ArenaTeardownResult
{
    ArenaHelperState helperState = ArenaHelperState::None;
    ArenaClosureState closureState = ArenaClosureState::None;
    uint32 cycleId = 0;
    uint32 instanceId = 0;
    uint32 staleSince = 0;
    bool dormantNoDemand = false;
    bool residualAttachment = false;
    bool staleDetachEligible = false;
    bool forceTeardown = false;
    bool quarantineActive = false;
    bool instanceHasRealPlayers = false;
    char const* blockStage = nullptr;
};

void BeginArenaTrackingCycle();
void ObserveArenaQueueParticipant(RandomPlayerbotMgr& mgr, Player* player, BattlegroundQueueTypeId queueTypeId,
    BattlegroundBracketId bracketId, bool isRated, bool isBot);
void FinalizeArenaTrackingCycle(RandomPlayerbotMgr& mgr, std::map<uint32, std::map<uint32, BattlegroundInfo>>& battlegroundData);
bool ArenaInstanceHasRealPlayers(BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, bool isRated, uint32 instanceId);

ArenaTeardownResult ObserveArenaTeardown(RandomPlayerbotMgr& mgr, Player* bot, uint32 desiredQueueType, BattlegroundBracketId bracketId,
    uint32 plannerPhase, bool hasRealDemand, uint32 plannerTeamNeed, bool desiredPresence, bool activeDesiredPresence,
    bool lifecycleOwned, bool worldReturnPending, bool retireWhenSafe, bool leaveRequested, std::string const& addData,
    uint32 staleTimeoutSeconds);

void MarkArenaHelperRetired(RandomPlayerbotMgr& mgr, ObjectGuid::LowType botId, std::string const& addData, char const* reason);
}

#endif
