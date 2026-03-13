#pragma once

#include "Battleground.h"
#include "RtgQueueLedger.h"

struct BattlegroundInfo;

namespace RTG
{
RtgHelperPurpose DetermineBgHelperPurpose(BattlegroundInfo const& bgInfo, TeamId teamId);
void SyncBgHelperState(Player* bot, uint32 desiredQueueType, BattlegroundBracketId bracketId, BattlegroundInfo const* bgInfo = nullptr);
void RecordHelperReservation(Player* bot, BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, TeamId preferredTeam,
                             char const* reason = nullptr, RtgHelperPurpose purpose = RtgHelperPurpose::StarterFill);
RtgLifecycleResult EvaluateRetire(Player* bot, uint32 retireRetrySeconds);
}
