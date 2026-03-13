#include "RtgQueueLifecycle.h"

#include "BattlegroundMgr.h"
#include "GameTime.h"
#include "Map.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotAIConfig.h"

namespace RTG
{
RtgHelperPurpose DetermineBgHelperPurpose(BattlegroundInfo const& bgInfo, TeamId teamId)
{
    uint32 queueReal = (teamId == TEAM_ALLIANCE) ? bgInfo.bgQueueAlliancePlayerCount : bgInfo.bgQueueHordePlayerCount;
    uint32 queueBots = (teamId == TEAM_ALLIANCE) ? bgInfo.bgQueueAllianceBotCount : bgInfo.bgQueueHordeBotCount;
    uint32 activeReal = (teamId == TEAM_ALLIANCE) ? bgInfo.bgActiveAlliancePlayerCount : bgInfo.bgActiveHordePlayerCount;
    uint32 activeBots = (teamId == TEAM_ALLIANCE) ? bgInfo.bgActiveAllianceBotCount : bgInfo.bgActiveHordeBotCount;
    if (activeReal + activeBots > queueReal + queueBots)
        return RtgHelperPurpose::LiveBalance;
    return RtgHelperPurpose::StarterFill;
}

void RecordHelperReservation(Player* bot, BattlegroundQueueTypeId queueTypeId, BattlegroundBracketId bracketId, TeamId preferredTeam,
                             char const* reason, RtgHelperPurpose purpose)
{
    if (!sPlayerbotAIConfig.rtgEventDriven || !sPlayerbotAIConfig.rtgQueueOwnershipEnable || !bot)
        return;

    RtgHelperLedgerEntry entry;
    entry.botGuid = bot->GetGUID().GetCounter();
    entry.accountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0;
    entry.isEventDrivenHelper = true;
    entry.state = RtgHelperState::LoggingIn;
    entry.ownerType = RtgHelperOwnerType::QueueDemand;
    entry.purpose = purpose;
    entry.target.bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    entry.target.queueTypeId = queueTypeId;
    entry.target.bracketId = bracketId;
    entry.target.preferredTeam = preferredTeam;
    entry.pendingQueueJoin = true;
    entry.creationReason = reason ? reason : "event-driven battleground helper login";
    RtgQueueLedger::Instance().Upsert(entry);
}

void SyncBgHelperState(Player* bot, uint32 desiredQueueType, BattlegroundBracketId bracketId, BattlegroundInfo const* bgInfo)
{
    if (!sPlayerbotAIConfig.rtgEventDriven || !sPlayerbotAIConfig.rtgQueueOwnershipEnable || !bot)
        return;

    uint32 botId = bot->GetGUID().GetCounter();
    RtgQueueLedger& ledger = RtgQueueLedger::Instance();
    RtgHelperLedgerEntry* entry = ledger.Get(botId);
    if (!entry)
        return;

    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(desiredQueueType);
    entry->target.queueTypeId = queueTypeId;
    entry->target.bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    entry->target.bracketId = bracketId;
    entry->target.preferredTeam = bot->GetTeamId();
    entry->pendingQueueJoin = false;
    entry->pendingBgJoin = false;

    if (bgInfo)
        entry->purpose = DetermineBgHelperPurpose(*bgInfo, bot->GetTeamId());

    uint32 nowMs = RTG::RTG_GetNowMs32();
    if (bot->IsInvitedForBattlegroundInstance())
    {
        ledger.AssignQueueOwnership(botId, entry->target, entry->purpose, "invited for battleground");
        ledger.MarkState(botId, RtgHelperState::Invited, "invited for battleground");
        entry->pendingBgJoin = true;
        if (sPlayerbotAIConfig.rtgQueueOwnershipProtectInBattleground)
            ledger.Protect(botId, nowMs + sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds * IN_MILLISECONDS, "invite protection");
        return;
    }

    if (bot->InBattleground() || bot->InArena() || (bot->GetMap() && bot->GetMap()->IsBattlegroundOrArena()))
    {
        uint32 instanceId = bot->GetInstanceId();
        ledger.AssignBattlegroundOwnership(botId, instanceId, "entered battleground");
        ledger.MarkState(botId, RtgHelperState::InBattleground, "entered battleground");
        if (sPlayerbotAIConfig.rtgQueueOwnershipProtectInBattleground)
            ledger.Protect(botId, nowMs + sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds * IN_MILLISECONDS, "battleground protection");
        return;
    }

    if (bot->InBattlegroundQueue() || bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
    {
        ledger.AssignQueueOwnership(botId, entry->target, entry->purpose, "queued for battleground");
        ledger.MarkState(botId, RtgHelperState::Queued, "queued for battleground");
        entry->pendingQueueJoin = true;
        if (sPlayerbotAIConfig.rtgQueueOwnershipProtectInBattleground)
            ledger.Protect(botId, nowMs + sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds * IN_MILLISECONDS, "queue protection");
        return;
    }

    if (entry->pendingRetire)
    {
        ledger.MarkState(botId, RtgHelperState::Releasing, "pending safe retire");
        return;
    }

    ledger.AssignQueueOwnership(botId, entry->target, entry->purpose, "idle queue helper");
    if (entry->state == RtgHelperState::LoggingIn || entry->state == RtgHelperState::Reserved || entry->state == RtgHelperState::Releasing ||
        entry->state == RtgHelperState::Retired)
        ledger.MarkState(botId, RtgHelperState::WorldIdle, "idle queue helper");
    else
        ledger.Touch(botId);
}

RtgLifecycleResult EvaluateRetire(Player* bot, uint32 retireRetrySeconds)
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

    uint32 now = RTG::RTG_GetNowMs32();
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
