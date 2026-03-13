#pragma once

#include "GameTime.h"
#include "PlayerbotAIConfig.h"
#include "RtgQueueTypes.h"

#include <unordered_map>
#include <vector>

namespace RTG
{
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
        uint32 now = RTG::RTG_GetNowMs32();
        if (!copy.createdAtMs)
            copy.createdAtMs = now;
        copy.updatedAtMs = now;
        _entries[copy.botGuid] = copy;
    }

    void Remove(ObjectGuid::LowType botGuid) { _entries.erase(botGuid); }

    void Touch(ObjectGuid::LowType botGuid)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
    }

    void MarkState(ObjectGuid::LowType botGuid, RtgHelperState newState, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->state = newState;
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
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
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
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
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void ClearOwnership(ObjectGuid::LowType botGuid, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->ownerType = RtgHelperOwnerType::None;
            entry->ownerInstanceId = 0;
            entry->pendingQueueJoin = false;
            entry->pendingBgJoin = false;
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void Protect(ObjectGuid::LowType botGuid, uint32 untilMs, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->protectedUntilMs = untilMs;
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
            if (reason)
                entry->creationReason = reason;
        }
    }

    void RequestRetire(ObjectGuid::LowType botGuid, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->pendingRetire = true;
            entry->retireRequestedAtMs = RTG::RTG_GetNowMs32();
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
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
        }
    }

    void Release(ObjectGuid::LowType botGuid, char const* reason = nullptr)
    {
        if (RtgHelperLedgerEntry* entry = Get(botGuid))
        {
            entry->pendingRetire = false;
            entry->ownerType = RtgHelperOwnerType::None;
            entry->ownerInstanceId = 0;
            entry->pendingQueueJoin = false;
            entry->pendingBgJoin = false;
            entry->state = RtgHelperState::Retired;
            entry->updatedAtMs = RTG::RTG_GetNowMs32();
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

    std::vector<ObjectGuid::LowType> GetTrackedBotIds() const
    {
        std::vector<ObjectGuid::LowType> result;
        result.reserve(_entries.size());
        for (auto const& kv : _entries)
            result.push_back(kv.first);
        return result;
    }

    std::vector<ObjectGuid::LowType> GetTransitionCandidates(uint32 maxTransitionMs) const
    {
        uint32 now = RTG::RTG_GetNowMs32();
        std::vector<ObjectGuid::LowType> result;
        for (auto const& kv : _entries)
        {
            RtgHelperLedgerEntry const& entry = kv.second;
            switch (entry.state)
            {
                case RtgHelperState::LoggingIn:
                case RtgHelperState::Queued:
                case RtgHelperState::Invited:
                case RtgHelperState::Releasing:
                    if (entry.updatedAtMs && now > entry.updatedAtMs && (now - entry.updatedAtMs) >= maxTransitionMs)
                        result.push_back(kv.first);
                    break;
                default:
                    break;
            }
        }
        return result;
    }

private:
    std::unordered_map<ObjectGuid::LowType, RtgHelperLedgerEntry> _entries;
};
}
