#pragma once

#include "GameTime.h"
#include "Player.h"
#include "SharedDefines.h"

#include <cstdint>
#include <string>

namespace RTG
{

static inline uint32 RTG_GetNowMs32()
{
    return static_cast<uint32>(GameTime::GetGameTimeMS().count());
}

enum class RtgHelperPurpose : uint8
{
    None = 0,
    StarterFill,
    LiveBalance
};

enum class RtgHelperState : uint8
{
    None = 0,
    Reserved,
    LoggingIn,
    WorldIdle,
    Queued,
    Invited,
    InBattleground,
    Releasing,
    Retired
};

enum class RtgHelperOwnerType : uint8
{
    None = 0,
    QueueDemand,
    Battleground
};

struct RtgBgTargetKey
{
    BattlegroundTypeId bgTypeId = BATTLEGROUND_TYPE_NONE;
    BattlegroundQueueTypeId queueTypeId = BATTLEGROUND_QUEUE_NONE;
    BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;
    TeamId preferredTeam = TEAM_NEUTRAL;
};

struct RtgHelperLedgerEntry
{
    ObjectGuid::LowType botGuid = 0;
    uint32 accountId = 0;
    RtgHelperPurpose purpose = RtgHelperPurpose::None;
    RtgHelperState state = RtgHelperState::None;
    RtgHelperOwnerType ownerType = RtgHelperOwnerType::None;
    RtgBgTargetKey target;
    uint32 ownerInstanceId = 0;
    uint32 createdAtMs = 0;
    uint32 updatedAtMs = 0;
    uint32 protectedUntilMs = 0;
    uint32 retireRequestedAtMs = 0;
    bool isEventDrivenHelper = false;
    bool pendingRetire = false;
    bool pendingQueueJoin = false;
    bool pendingBgJoin = false;
    std::string creationReason;
};

enum class RtgLifecycleDecision : uint8
{
    Allow = 0,
    Delay,
    Deny
};

struct RtgLifecycleResult
{
    RtgLifecycleDecision decision = RtgLifecycleDecision::Deny;
    uint32 retryAfterMs = 0;
    std::string reason;
};
}
