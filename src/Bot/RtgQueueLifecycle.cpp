#include "RtgQueueLifecycle.h"

#include "RtgArenaState.h"
#include "RtgQueueMetadata.h"
#include "BattlegroundMgr.h"
#include "GameTime.h"
#include "Map.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotAIConfig.h"

namespace RTG
{
namespace
{
static bool RTG_IsArenaQueueType(BattlegroundQueueTypeId queueTypeId)
{
    uint32 raw = uint32(BattlegroundMgr::BGArenaType(queueTypeId));
    if (raw != 0u)
        return true;
    if (queueTypeId == BATTLEGROUND_QUEUE_2v2 || queueTypeId == BATTLEGROUND_QUEUE_3v3 || queueTypeId == BATTLEGROUND_QUEUE_5v5)
        return true;
    return uint32(queueTypeId) == 9u;
}

static bool RTG_HasQueueSlotForType(Player* bot, BattlegroundQueueTypeId queueTypeId)
{
    if (!bot || queueTypeId <= BATTLEGROUND_QUEUE_NONE || queueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
        return false;

    for (uint8 queueSlot = 0; queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueSlot)
    {
        if (bot->GetBattlegroundQueueTypeId(queueSlot) == queueTypeId)
            return true;
    }

    return false;
}

static bool RTG_IsActiveInQueueType(Player* bot, BattlegroundQueueTypeId queueTypeId)
{
    if (!bot || queueTypeId <= BATTLEGROUND_QUEUE_NONE || queueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
        return false;

    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
        return false;

    if ((bot->InBattleground() || bot->InArena()) && bot->GetBattlegroundTypeId() == bgTypeId)
        return true;

    if (!RTG_IsArenaQueueType(queueTypeId))
        if (Battleground* bg = bot->GetBattleground())
            return bg->GetBgTypeID() == bgTypeId;

    return false;
}

static std::string RTG_MakeDemandKey(BattlegroundQueueTypeId queueTypeId, uint32 queueType, uint32 bracketId)
{
    return RTG_IsArenaQueueType(queueTypeId) ?
        (std::string("rtg_arena_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId)) :
        (std::string("rtg_bg_real_demand:") + std::to_string(queueType) + ":" + std::to_string(bracketId));
}

static std::string RTG_MakePhaseKey(BattlegroundQueueTypeId queueTypeId, uint32 queueType, uint32 bracketId)
{
    return RTG_IsArenaQueueType(queueTypeId) ?
        (std::string("rtg_arena_phase:") + std::to_string(queueType) + ":" + std::to_string(bracketId)) :
        (std::string("rtg_bg_phase:") + std::to_string(queueType) + ":" + std::to_string(bracketId));
}

static std::string RTG_MakeTeamNeedKey(BattlegroundQueueTypeId queueTypeId, uint32 queueType, uint32 bracketId, uint32 teamId)
{
    return RTG_IsArenaQueueType(queueTypeId) ?
        (std::string("rtg_arena_team_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId) + ":" + std::to_string(teamId)) :
        (std::string("rtg_bg_team_need:") + std::to_string(queueType) + ":" + std::to_string(bracketId) + ":" + std::to_string(teamId));
}

static char const* RTG_PendingKey(BattlegroundQueueTypeId queueTypeId)
{
    return RTG_IsArenaQueueType(queueTypeId) ? "rtg_arena_pending" : "rtg_bg_pending";
}

static char const* RTG_QueueGraceKey(BattlegroundQueueTypeId queueTypeId)
{
    return RTG_IsArenaQueueType(queueTypeId) ? "rtg_arena_queue_grace" : "rtg_bg_queue_grace";
}

static bool RTG_IsActivelyOwnedHelperState(RtgHelperState state)
{
    switch (state)
    {
        case RtgHelperState::Reserved:
        case RtgHelperState::LoggingIn:
        case RtgHelperState::WorldIdle:
        case RtgHelperState::Queued:
        case RtgHelperState::Invited:
        case RtgHelperState::InBattleground:
            return true;
        default:
            return false;
    }
}

static bool RTG_IsOrphanQueuedBgHelper(Player* bot, RtgHelperLedgerEntry const& entry)
{
    if (!bot)
        return false;

    if (!RTG_HasQueueSlotForType(bot, entry.target.queueTypeId))
        return false;

    if (bot->InBattleground() || bot->InArena() || (bot->GetMap() && bot->GetMap()->IsBattlegroundOrArena()))
        return false;

    if (entry.target.queueTypeId <= BATTLEGROUND_QUEUE_NONE)
        return false;

    uint32 queueType = uint32(entry.target.queueTypeId);
    uint32 bracketId = uint32(entry.target.bracketId);
    std::string demandKey = RTG_MakeDemandKey(entry.target.queueTypeId, queueType, bracketId);
    std::string phaseKey = RTG_MakePhaseKey(entry.target.queueTypeId, queueType, bracketId);
    if (sRandomPlayerbotMgr.RTG_GetBotEventValue(0, demandKey) != 0)
        return false;

    if (sRandomPlayerbotMgr.RTG_GetBotEventValue(0, phaseKey) != 0)
        return false;

    if (sRandomPlayerbotMgr.RTG_GetBotEventValue(bot->GetGUID().GetCounter(), RTG_PendingKey(entry.target.queueTypeId)) != 0)
        return false;

    return true;
}

static bool RTG_HasActualPvpLifecycle(Player* bot, BattlegroundQueueTypeId queueTypeId = BATTLEGROUND_QUEUE_NONE)
{
    if (!bot)
        return false;

    if (queueTypeId <= BATTLEGROUND_QUEUE_NONE || queueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
        return bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue() || bot->IsInvitedForBattlegroundInstance();

    if (RTG_HasQueueSlotForType(bot, queueTypeId))
        return true;

    if (bot->IsInvitedForBattlegroundInstance() && RTG_HasQueueSlotForType(bot, queueTypeId))
        return true;

    if (RTG_IsActiveInQueueType(bot, queueTypeId))
        return true;

    return false;
}

static bool RTG_HelperHasOutstandingDemand(Player* bot, RtgHelperLedgerEntry const& entry)
{
    if (!bot)
        return false;

    ObjectGuid::LowType botId = bot->GetGUID().GetCounter();
    if (sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_lfg_pending") ||
        sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_dungeon_active"))
        return true;

    std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(botId, "add");
    bool arenaManaged = IsArenaManagedAddData(addData);
    if (arenaManaged)
    {
        bool arenaDraining =
            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_arena_retire_when_safe") != 0 ||
            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_arena_world_return_since") != 0 ||
            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_arena_leave_requested") != 0 ||
            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_arena_logout_queued") != 0 ||
            GetArenaClosureState(sRandomPlayerbotMgr, botId) == ArenaClosureState::Pending ||
            IsArenaTeardownQuarantined(sRandomPlayerbotMgr, botId);

        if (arenaDraining)
            return false;
    }

    if (!IsQueueManagedAddData(addData))
    {
        if (sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_bg_pending") ||
            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, "rtg_bg_queue_grace") ||
            entry.pendingQueueJoin || entry.pendingBgJoin)
            return true;

        return entry.ownerType == RtgHelperOwnerType::QueueDemand && RTG_IsActivelyOwnedHelperState(entry.state);
    }

    uint32 team = 0;
    uint32 level = 0;
    uint32 queueType = 0;
    uint32 owner = 0;
    bool parsedBgAddData = ParseBgAddData(addData, team, level, queueType, &owner);
    if (parsedBgAddData)
    {
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(queueType));
        if (bgTypeId != BATTLEGROUND_TYPE_NONE)
        {
            if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId))
            {
                if (PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), level ? level : bot->GetLevel()))
                {
                    uint32 bracketId = uint32(pvpDiff->GetBracketId());
                    BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueType);
                    bool isArenaQueue = RTG_IsArenaQueueType(queueTypeId);
                    bool ownerBoundArenaHelper = arenaManaged && owner != 0;
                    std::string demandKey = RTG_MakeDemandKey(queueTypeId, queueType, bracketId);
                    std::string phaseKey = RTG_MakePhaseKey(queueTypeId, queueType, bracketId);
                    std::string teamNeedKey = RTG_MakeTeamNeedKey(queueTypeId, queueType, bracketId, team);
                    bool queueDemandActive =
                        sRandomPlayerbotMgr.RTG_GetBotEventValue(0, demandKey) != 0 ||
                        sRandomPlayerbotMgr.RTG_GetBotEventValue(0, phaseKey) != 0 ||
                        sRandomPlayerbotMgr.RTG_GetBotEventValue(0, teamNeedKey) != 0;

                    if (queueDemandActive)
                    {
                        bool helperHasLiveArenaTransition =
                            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, RTG_PendingKey(queueTypeId)) != 0 ||
                            sRandomPlayerbotMgr.RTG_GetBotEventValue(botId, RTG_QueueGraceKey(queueTypeId)) != 0 ||
                            entry.pendingQueueJoin ||
                            entry.pendingBgJoin ||
                            RTG_HasActualPvpLifecycle(bot, queueTypeId) ||
                            RTG_HasQueueSlotForType(bot, queueTypeId) ||
                            bot->IsInvitedForBattlegroundInstance() ||
                            RTG_IsActiveInQueueType(bot, queueTypeId);

                        if (isArenaQueue || ownerBoundArenaHelper)
                            return helperHasLiveArenaTransition;

                        return true;
                    }

                    return entry.ownerType == RtgHelperOwnerType::QueueDemand &&
                           entry.target.queueTypeId == BattlegroundQueueTypeId(queueType) &&
                           entry.target.bracketId == pvpDiff->GetBracketId() &&
                           entry.state == RtgHelperState::InBattleground &&
                           RTG_IsActiveInQueueType(bot, queueTypeId);
                }
            }
        }

        return entry.ownerType == RtgHelperOwnerType::QueueDemand &&
               entry.state == RtgHelperState::InBattleground &&
               RTG_IsActiveInQueueType(bot, entry.target.queueTypeId);
    }

    if (!parsedBgAddData && (entry.pendingQueueJoin || entry.pendingBgJoin))
        return true;

    return entry.ownerType == RtgHelperOwnerType::QueueDemand && RTG_IsActivelyOwnedHelperState(entry.state);
}
}

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
    entry.state = RtgHelperState::Reserved;
    entry.ownerType = RtgHelperOwnerType::QueueDemand;
    entry.purpose = purpose;
    entry.target.bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    entry.target.queueTypeId = queueTypeId;
    entry.target.bracketId = bracketId;
    entry.target.preferredTeam = preferredTeam;
    entry.pendingQueueJoin = true;
    entry.creationReason = reason ? reason : "event-driven battleground helper login";
    entry.protectedUntilMs = RTG_GetNowMs32() + std::max<uint32>(20u, sPlayerbotAIConfig.rtgQueueGraceSeconds) * IN_MILLISECONDS;
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
    bool validQueueTarget = queueTypeId > BATTLEGROUND_QUEUE_NONE && queueTypeId < MAX_BATTLEGROUND_QUEUE_TYPES;
    bool queuedState = validQueueTarget ? RTG_HasQueueSlotForType(bot, queueTypeId) : bot->InBattlegroundQueue();
    bool invitedState = bot->IsInvitedForBattlegroundInstance() && (!validQueueTarget || queuedState);
    bool activeInstanceState = validQueueTarget ? RTG_IsActiveInQueueType(bot, queueTypeId) : (bot->InBattleground() || bot->InArena());
    bool mapOnlyResidue = !invitedState && !activeInstanceState && !queuedState &&
                          !validQueueTarget && bot->GetMap() && bot->GetMap()->IsBattlegroundOrArena();

    if (invitedState)
    {
        ledger.AssignQueueOwnership(botId, entry->target, entry->purpose, "invited for battleground");
        ledger.MarkState(botId, RtgHelperState::Invited, "invited for battleground");
        entry->pendingBgJoin = true;
        if (BattlegroundMgr::BGArenaType(queueTypeId) != 0 || uint32(queueTypeId) == 9u)
        {
            LOG_INFO("playerbots", "[RTG][ARENA][POP] helper={} queue={} bracket={} team={} state=invited",
                botId, uint32(queueTypeId), uint32(bracketId), uint32(bot->GetTeamId()));
        }
        if (sPlayerbotAIConfig.rtgQueueOwnershipProtectInBattleground)
            ledger.Protect(botId, nowMs + sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds * IN_MILLISECONDS, "invite protection");
        return;
    }

    if (activeInstanceState)
    {
        uint32 instanceId = bot->GetInstanceId();
        ledger.AssignBattlegroundOwnership(botId, instanceId, "entered battleground");
        ledger.MarkState(botId, RtgHelperState::InBattleground, "entered battleground");
        if (BattlegroundMgr::BGArenaType(queueTypeId) != 0 || uint32(queueTypeId) == 9u)
        {
            LOG_INFO("playerbots", "[RTG][ARENA][POP] helper={} queue={} bracket={} team={} state=entered instance={}",
                botId, uint32(queueTypeId), uint32(bracketId), uint32(bot->GetTeamId()), instanceId);
        }
        if (sPlayerbotAIConfig.rtgQueueOwnershipProtectInBattleground)
            ledger.Protect(botId, nowMs + sPlayerbotAIConfig.rtgQueueOwnershipRetireRetrySeconds * IN_MILLISECONDS, "battleground protection");
        return;
    }

    if (mapOnlyResidue)
    {
        bool transitionProtected = entry->protectedUntilMs && nowMs < entry->protectedUntilMs;
        if (entry->pendingRetire || !transitionProtected)
        {
            ledger.ClearOwnership(botId, "map-only battleground residue");
            ledger.MarkState(botId, entry->pendingRetire ? RtgHelperState::Releasing : RtgHelperState::WorldIdle,
                entry->pendingRetire ? "map-only battleground residue pending retire" : "map-only battleground residue");
            return;
        }

        uint32 instanceId = bot->GetInstanceId();
        ledger.AssignBattlegroundOwnership(botId, instanceId, "map-only battleground transition");
        ledger.MarkState(botId, RtgHelperState::InBattleground, "map-only battleground transition");
        return;
    }

    if (queuedState)
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

    bool orphanQueuedBgHelper = RTG_IsOrphanQueuedBgHelper(bot, *entry);
    bool actualPvpLifecycle = RTG_HasActualPvpLifecycle(bot, entry->target.queueTypeId);

    if ((RTG_HelperHasOutstandingDemand(bot, *entry) && !orphanQueuedBgHelper) ||
        ((entry->ownerType == RtgHelperOwnerType::Battleground ||
          entry->state == RtgHelperState::InBattleground ||
          entry->state == RtgHelperState::Invited ||
          (entry->state == RtgHelperState::Queued && !orphanQueuedBgHelper)) && actualPvpLifecycle) ||
        (entry->state == RtgHelperState::LoggingIn &&
         (actualPvpLifecycle || entry->pendingQueueJoin || entry->pendingBgJoin)))
    {
        result.decision = RtgLifecycleDecision::Delay;
        result.retryAfterMs = retireRetrySeconds * IN_MILLISECONDS;
        result.reason = (RTG_HelperHasOutstandingDemand(bot, *entry) && !orphanQueuedBgHelper) ? "helper still owned by active RTG queue demand" : "helper still lifecycle-owned by battleground state";
        return result;
    }

    result.decision = RtgLifecycleDecision::Allow;
    result.reason = orphanQueuedBgHelper ? "orphan queued helper with no real demand" : "safe to retire";
    return result;
}

void RegisterPendingHelperLogin(ObjectGuid::LowType botGuid, uint32 accountId, std::string const& addData)
{
    if (!botGuid || addData.empty() || !RTG::IsQueueManagedAddData(addData))
        return;

    RtgHelperLedgerEntry entry;
    entry.botGuid = botGuid;
    entry.accountId = accountId;
    entry.isEventDrivenHelper = true;
    entry.pendingQueueJoin = true;
    entry.state = RtgHelperState::Reserved;
    entry.ownerType = RtgHelperOwnerType::QueueDemand;
    entry.creationReason = "pending helper acquisition";

    uint32 team = 0;
    uint32 level = 0;
    uint32 queueType = 0;
    uint32 owner = 0;
    if (RTG::ParseBgAddData(addData, team, level, queueType, &owner))
    {
        entry.purpose = RtgHelperPurpose::StarterFill;
        entry.target.queueTypeId = BattlegroundQueueTypeId(queueType);
        entry.target.bgTypeId = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(queueType));
        entry.target.preferredTeam = TeamId(team);
        if (entry.target.bgTypeId != BATTLEGROUND_TYPE_NONE)
        {
            if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(entry.target.bgTypeId))
            {
                if (PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), level))
                    entry.target.bracketId = pvpDiff->GetBracketId();
            }
        }
        entry.creationReason = owner ? (std::string("pending bg helper owner=") + std::to_string(owner)) : "pending bg helper";
    }
    else if (RTG::ParseLfgAddData(addData, team, level, nullptr, &owner))
    {
        entry.purpose = RtgHelperPurpose::StarterFill;
        entry.target.preferredTeam = TeamId(team);
        entry.creationReason = owner ? (std::string("pending lfg helper owner=") + std::to_string(owner)) : "pending lfg helper";
    }
    else
    {
        return;
    }

    RtgQueueLedger::Instance().Upsert(entry);
}
}
