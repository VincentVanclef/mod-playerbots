#pragma once

#include "GameTime.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTG
{
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

class RtgQueueLedger
{
public:
    static RtgQueueLedger& Instance()
    {
        static RtgQueueLedger instance;
        return instance;
    }

    RtgHelperLedgerEntry* Get(ObjectGuid::LowType botGuid)
    {
        auto itr = _entries.find(botGuid);
        return itr != _entries.end() ? &itr->second : nullptr;
    }

    RtgHelperLedgerEntry const* Get(ObjectGuid::LowType botGuid) const
    {
        auto itr = _entries.find(botGuid);
        return itr != _entries.end() ? &itr->second : nullptr;
    }

    bool Has(ObjectGuid::LowType botGuid) const { return _entries.find(botGuid) != _entries.end(); }

    void Upsert(RtgHelperLedgerEntry const& entry)
    {
        RtgHelperLedgerEntry copy = entry;
        uint32 now = GameTime::GetGameTimeMS();
        if (!copy.createdAtMs)
            copy.createdAtMs = now;
        copy.updatedAtMs = now;
        _entries[copy.botGuid] = copy;
    }

    void Remove(ObjectGuid::LowType botGuid) { _entries.erase(botGuid); }

    void Touch(ObjectGuid::LowType botGuid)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
            entry->updatedAtMs = GameTime::GetGameTimeMS();
    }

    void MarkState(ObjectGuid::LowType botGuid, RtgHelperState newState, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->state = newState;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void AssignQueueOwnership(ObjectGuid::LowType botGuid, RtgBgTargetKey const& key, RtgHelperPurpose purpose, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->ownerType = RtgHelperOwnerType::QueueDemand;
            entry->target = key;
            entry->purpose = purpose;
            entry->ownerInstanceId = 0;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void AssignBattlegroundOwnership(ObjectGuid::LowType botGuid, uint32 instanceId, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->ownerType = RtgHelperOwnerType::Battleground;
            entry->ownerInstanceId = instanceId;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void Protect(ObjectGuid::LowType botGuid, uint32 untilMs, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->protectedUntilMs = untilMs;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void RequestRetire(ObjectGuid::LowType botGuid, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->pendingRetire = true;
            entry->retireRequestedAtMs = GameTime::GetGameTimeMS();
            entry->state = RtgHelperState::Releasing;
            entry->updatedAtMs = entry->retireRequestedAtMs;
            if (reason)
                entry->creationReason = reason;
        }
    }

    void ClearRetireRequest(ObjectGuid::LowType botGuid)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->pendingRetire = false;
            entry->retireRequestedAtMs = 0;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
        }
    }

    void Release(ObjectGuid::LowType botGuid, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->pendingRetire = false;
            entry->ownerType = RtgHelperOwnerType::None;
            entry->ownerInstanceId = 0;
            entry->state = RtgHelperState::Retired;
            entry->updatedAtMs = GameTime::GetGameTimeMS();
            if (reason)
                entry->creationReason = reason;
        }
    }

    std::vector<ObjectGuid::LowType> GetRetireCandidates() const
    {
        std::vector<ObjectGuid::LowType> result;
        for (auto const& kv : _entries)
            if (kv.second.pendingRetire)
                result.push_back(kv.first);
        return result;
    }

private:
    std::unordered_map<ObjectGuid::LowType, RtgHelperLedgerEntry> _entries;
};

inline RtgLifecycleResult EvaluateRetire(Player* bot, uint32 retireRetrySeconds)
{
    RtgLifecycleResult result;
    if (!bot)
    {
        result.decision = RtgLifecycleDecision::Deny;
        result.reason = "bot missing";
        return result;
    }

    RtgHelperLedgerEntry const* entry = RtgQueueLedger::Instance().Get(bot->GetGUID().GetCounter());
    if (!entry)
    {
        result.decision = RtgLifecycleDecision::Allow;
        result.reason = "no ledger entry";
        return result;
    }

    uint32 now = GameTime::GetGameTimeMS();
    if (entry->protectedUntilMs && now < entry->protectedUntilMs)
    {
        result.decision = RtgLifecycleDecision::Delay;
        result.retryAfterMs = retireRetrySeconds * IN_MILLISECONDS;
        result.reason = "protection window active";
        return result;
    }

    if (entry->ownerType == RtgHelperOwnerType::Battleground ||
        entry->state == RtgHelperState::InBattleground ||
        entry->state == RtgHelperState::Invited ||
        entry->state == RtgHelperState::Queued ||
        entry->state == RtgHelperState::LoggingIn)
    {
        result.decision = RtgLifecycleDecision::Delay;
        result.retryAfterMs = retireRetrySeconds * IN_MILLISECONDS;
        result.reason = "helper still lifecycle-owned by battleground state";
        return result;
    }

    result.decision = RtgLifecycleDecision::Allow;
    result.reason = "safe to retire";
    return result;
}
}
