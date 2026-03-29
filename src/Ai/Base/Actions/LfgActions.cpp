/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgActions.h"

#include "AiFactory.h"
#include "ItemVisitors.h"
#include "LFGMgr.h"
#include "LFGPackets.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "RtgRdfRoleResolver.h"
#include "World.h"
#include "WorldPacket.h"

#include <unordered_map>
#include <ctime>

using namespace lfg;

namespace
{

    static bool RTG_LfgDebugEnabled()
    {
        return sPlayerbotAIConfig.rtgEventDriven && sPlayerbotAIConfig.rtgEventDebug;
    }

    static bool RTG_HasPrefix(std::string const& value, std::string const& prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    static bool RTG_ParseLfgAssignment(std::string const& data, uint32& team, uint32& level, uint32& role, uint32& owner)
    {
        team = 0;
        level = 0;
        role = 0;
        owner = 0;
        if (!RTG_HasPrefix(data, "rtg_lfg:"))
            return false;

        std::string payload = data.substr(8);
        size_t sep1 = payload.find(':');
        size_t sep2 = payload.find(':', sep1 == std::string::npos ? sep1 : sep1 + 1);
        size_t sep3 = payload.find(':', sep2 == std::string::npos ? sep2 : sep2 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos || sep3 == std::string::npos)
            return false;

        try
        {
            team = static_cast<uint32>(std::stoul(payload.substr(0, sep1)));
            level = static_cast<uint32>(std::stoul(payload.substr(sep1 + 1, sep2 - sep1 - 1)));
            role = static_cast<uint32>(std::stoul(payload.substr(sep2 + 1, sep3 - sep2 - 1)));
            owner = static_cast<uint32>(std::stoul(payload.substr(sep3 + 1)));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool RTG_ParseLfgDesiredRole(std::string const& data, uint32& role)
    {
        uint32 team = 0;
        uint32 level = 0;
        uint32 owner = 0;
        return RTG_ParseLfgAssignment(data, team, level, role, owner);
    }

    static uint32 RTG_ActualRoleForBot(Player* bot)
    {
        return RTG::ActualRoleForBot(bot);
    }

    static uint32 RTG_ActualSpecRoleMask(Player* bot)
    {
        return RTG::GetActualSpecRoleMask(bot);
    }

    static bool RTG_ActualSpecCanPerformRole(Player* bot, uint32 role)
    {
        return RTG::ActualSpecCanPerformRole(bot, role);
    }

    static std::string RTG_RoleMismatchCooldownKey(uint32 desiredRole)
    {
        return std::string("rtg_lfg_role_block:") + std::to_string(desiredRole);
    }

    static void RTG_RecordRuntimeRole(Player* bot, uint32 actualRole)
    {
        if (!bot || !actualRole)
            return;

        sRandomPlayerbotMgr.RTG_SetBotEventValue(bot->GetGUID().GetCounter(), "rtg_runtime_lfg_role", actualRole, 86400);
    }

    static void RTG_BlockDesiredRole(Player* bot, uint32 desiredRole)
    {
        if (!bot || !desiredRole)
            return;

        sRandomPlayerbotMgr.RTG_SetBotEventValue(bot->GetGUID().GetCounter(), RTG_RoleMismatchCooldownKey(desiredRole), 1u, 900);
    }

    static bool RTG_GroupHasRealPlayerMember(Player* bot)
    {
        if (!bot)
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;

            if (!GET_PLAYERBOT_AI(member))
                return true;
        }

        return false;
    }

    static bool RTG_IsQueuedLfgBot(Player* bot)
    {
        return RTG_HasPrefix(sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add"), "rtg_lfg:");
    }

    static bool RTG_IsAssignedLfgHelper(Player* bot, uint32* desiredRole = nullptr)
    {
        if (!bot)
            return false;

        uint32 role = 0;
        bool assigned = RTG_ParseLfgDesiredRole(sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add"), role);
        if (desiredRole)
            *desiredRole = role;
        return assigned;
    }

    static bool RTG_AssignedLfgHelperHasDemand(Player* bot)
    {
        if (!bot || !sPlayerbotAIConfig.rtgEventDriven)
            return false;

        uint32 team = 0;
        uint32 level = 0;
        uint32 role = 0;
        uint32 owner = 0;
        std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add");
        if (!RTG_ParseLfgAssignment(addData, team, level, role, owner))
            return false;

        if (owner && sRandomPlayerbotMgr.RTG_GetBotEventValue(owner, "rtg_lfg_real_demand") != 0)
            return true;
        if (sRandomPlayerbotMgr.RTG_GetGlobalEvent("rtg_lfg_need_total") != 0)
            return true;
        if (sRandomPlayerbotMgr.RTG_GetBotEventValue(bot->GetGUID().GetCounter(), "rtg_dungeon_active") != 0)
            return true;

        return false;
    }

    static void RTG_ClearDungeonQueuePenalties(Player* bot)
    {
        if (!bot)
            return;

        bot->RemoveAura(71041);
        bot->RemoveAura(71328);
    }

    struct RTG_LfgProposalPacketData
    {
        uint32 dungeonEntry = 0;
        uint8 state = 0;
        uint32 proposalId = 0;
        uint32 encounters = 0;
        uint8 silent = 0;
        uint8 groupSize = 0;
    };

    static bool RTG_ParseProposalPacket(WorldPacket const& source, RTG_LfgProposalPacketData& data)
    {
        if (source.size() < (sizeof(uint32) + sizeof(uint8) + sizeof(uint32) + sizeof(uint32) + sizeof(uint8) + sizeof(uint8)))
            return false;

        WorldPacket p(source);
        p.rpos(0);
        p >> data.dungeonEntry;
        p >> data.state;
        p >> data.proposalId;
        p >> data.encounters;
        p >> data.silent;
        p >> data.groupSize;
        return true;
    }

    static uint32 RTG_GetAcceptedProposal(Player* bot)
    {
        return bot ? sRandomPlayerbotMgr.RTG_GetBotEventValue(bot->GetGUID().GetCounter(), "rtg_lfg_accept_proposal") : 0;
    }

}

bool LfgJoinAction::Execute(Event event) { return JoinLFG(); }

uint32 LfgJoinAction::GetRoles()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        if (botAI->IsTank(bot))
            return lfg::PLAYER_ROLE_TANK;
        if (botAI->IsHeal(bot))
            return lfg::PLAYER_ROLE_HEALER;
        return lfg::PLAYER_ROLE_DAMAGE;
    }

    uint32 actualRole = RTG_ActualRoleForBot(bot);
    uint32 actualRoleMask = RTG_ActualSpecRoleMask(bot);
    uint32 botId = bot->GetGUID().GetCounter();

    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 desiredRole = 0;
        std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add");
        if (RTG_ParseLfgDesiredRole(addData, desiredRole) && desiredRole)
        {
            if (!RTG::ClassCanRole(bot->getClass(), desiredRole))
            {
                if (RTG_LfgDebugEnabled())
                    LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} class={} specTab={} verdict=class_cannot_role usingActualRole", botId, desiredRole, actualRole, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
                return actualRole;
            }

            if (!RTG::ActualSpecCanPerformRole(bot, desiredRole))
            {
                LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} actualMask={} class={} specTab={} verdict=runtime_incompatible_usingActualRole", botId, desiredRole, actualRole, actualRoleMask, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
                return actualRole;
            }

            if (actualRoleMask && actualRoleMask != desiredRole && (actualRoleMask & desiredRole) == desiredRole)
            {
                LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} actualMask={} class={} specTab={} verdict=flex_capable_assigned_role", botId, desiredRole, actualRole, actualRoleMask, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
            }

            if (desiredRole != actualRole)
            {
                LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} actualMask={} class={} specTab={} verdict=planner_role_override", botId, desiredRole, actualRole, actualRoleMask, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
            }
            else if (RTG_LfgDebugEnabled())
            {
                LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} actualMask={} class={} specTab={} verdict=aligned", botId, desiredRole, actualRole, actualRoleMask, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
            }

            return desiredRole;
        }
    }

    return actualRoleMask ? actualRoleMask : actualRole;
}

bool LfgJoinAction::JoinLFG()
{
    static std::unordered_map<uint32, time_t> rtgNextJoinAttempt;

    RTG_ClearDungeonQueuePenalties(bot);

    // check if already in lfg
    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} blocked existingState={}", bot->GetGUID().GetCounter(), uint32(state));
        rtgNextJoinAttempt.erase(bot->GetGUID().GetCounter());
        return false;
    }

    time_t now = time(nullptr);
    uint32 botId = bot->GetGUID().GetCounter();

    auto attemptIt = rtgNextJoinAttempt.find(botId);
    if (attemptIt != rtgNextJoinAttempt.end() && now < attemptIt->second)
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} cooldown waitUntil={} now={}", botId, uint32(attemptIt->second), uint32(now));
        return false;
    }

    // ------------------------------------------------------------------
    // RTG: Event-driven LFG grace
    // Only let bots queue for LFG after real players have initiated LFG queueing
    // and the grace window has expired.
    // ------------------------------------------------------------------
    if (RTG_GroupHasRealPlayerMember(bot))
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} blocked real-player-group", botId);
        return false;
    }

    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 desiredRole = 0;
        bool assignedHelper = RTG_IsAssignedLfgHelper(bot, &desiredRole);

        if (!assignedHelper)
        {
            uint32 start = sRandomPlayerbotMgr.RTG_GetGlobalEvent("rtg_lfg_start");
            if (!start)
            {
                if (RTG_LfgDebugEnabled())
                    LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} waiting no-global-start", botId);
                return false;
            }

            if (now < (time_t)(start + sPlayerbotAIConfig.rtgQueueGraceSeconds))
            {
                if (RTG_LfgDebugEnabled())
                    LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} waiting grace start={} grace={} now={}", botId, start, sPlayerbotAIConfig.rtgQueueGraceSeconds, uint32(now));
                return false;
            }
        }
        else
        {
            if (!RTG_AssignedLfgHelperHasDemand(bot))
            {
                if (RTG_LfgDebugEnabled())
                    LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} blocked no-active-demand", botId);
                return false;
            }

            RTG_ClearDungeonQueuePenalties(bot);

            uint32 desiredRole = 0;
            std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(botId, "add");
            if (RTG_ParseLfgDesiredRole(addData, desiredRole) && desiredRole)
            {
                if (!RTG::ClassCanRole(bot->getClass(), desiredRole))
                {
                    LOG_INFO("playerbots", "[RTG][LFG][ROLE] helper={} desiredRole={} actualRole={} class={} specTab={} verdict=logout_class_incompatible_join", botId, desiredRole, RTG_ActualRoleForBot(bot), bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
                    sRandomPlayerbotMgr.RTG_ClearQueueHelperState(botId);
                    rtgNextJoinAttempt.erase(botId);
                    sRandomPlayerbotMgr.RTG_RequestQueueHelperLogout(bot->GetGUID(), "rtg_lfg_role_class_incompatible_join");
                    return false;
                }

                uint32 actualRole = RTG_ActualRoleForBot(bot);
                if (!RTG_ActualSpecCanPerformRole(bot, desiredRole))
                {
                    LOG_INFO("playerbots", "[RTG][RDF][FAIL] helper={} owner={} reason=runtime_role_mismatch desiredRole={} actualRole={} class={} specTab={}",
                        botId, 0u, desiredRole, actualRole, bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
                    sRandomPlayerbotMgr.RTG_ClearQueueHelperState(botId);
                    rtgNextJoinAttempt.erase(botId);
                    sRandomPlayerbotMgr.RTG_RequestQueueHelperLogout(bot->GetGUID(), "rtg_lfg_runtime_role_mismatch");
                    return false;
                }

                uint32 currentRoles = sLFGMgr->GetRoles(bot->GetGUID());
                if (currentRoles != desiredRole)
                {
                    if (bot->GetGroup())
                    {
                        WorldPacket* rolePacket = new WorldPacket(CMSG_LFG_SET_ROLES);
                        *rolePacket << (uint8)desiredRole;
                        bot->GetSession()->QueuePacket(rolePacket);
                        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_pending", 1, 20, addData);
                        rtgNextJoinAttempt[botId] = now + 1;
                        if (RTG_LfgDebugEnabled())
                            LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} waiting role_sync desiredRole={} currentRoles={} grouped=1", botId, desiredRole, currentRoles);
                        return false;
                    }

                    if (RTG_IsQueuedLfgBot(bot))
                    {
                        LOG_INFO("playerbots", "[RTG][RDF][JOIN] helper={} desiredRole={} currentRoles={} roleSync=join_packet grouped=0", botId, desiredRole, currentRoles);
                    }
                }
            }

            rtgNextJoinAttempt[botId] = now + 1;
        }
    }

    /*ItemCountByQuality visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_EQUIP);
    bool random = urand(0, 100) < 20;
    bool heroic = urand(0, 100) < 50 &&
                  (visitor.count[ITEM_QUALITY_EPIC] >= 3 || visitor.count[ITEM_QUALITY_RARE] >= 10) &&
                  bot->GetLevel() >= 70;
    bool rbotAId = !heroic && (urand(0, 100) < 50 && visitor.count[ITEM_QUALITY_EPIC] >= 5 &&
                               (bot->GetLevel() == 60 || bot->GetLevel() == 70 || bot->GetLevel() == 80));*/

    LfgDungeonSet list;
    std::vector<uint32> selected;

    std::vector<uint32> dungeons = sRandomPlayerbotMgr.LfgDungeons[bot->GetTeamId()];
    if (!dungeons.size())
        return false;

    for (std::vector<uint32>::iterator i = dungeons.begin(); i != dungeons.end(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*i);
        if (!dungeon || (dungeon->TypeID != LFG_TYPE_RANDOM && dungeon->TypeID != LFG_TYPE_DUNGEON &&
                         dungeon->TypeID != LFG_TYPE_HEROIC && dungeon->TypeID != LFG_TYPE_RAID))
            continue;

        auto const& botLevel = bot->GetLevel();

        /*LFG_TYPE_RANDOM on classic is 15-58 so bot over level 25 will never queue*/
        if (dungeon->MinLevel && (botLevel < dungeon->MinLevel || botLevel > dungeon->MaxLevel) ||
            (botLevel > dungeon->MinLevel + 10 && dungeon->TypeID == LFG_TYPE_DUNGEON))
            continue;

        selected.push_back(dungeon->ID);
        list.insert(dungeon->ID);
    }

    if (!selected.size())
        return false;

    if (list.empty())
        return false;

    bool many = list.size() > 1;
    LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*list.begin());

    // check role for console msg
    std::string _roles = "multiple roles";
    uint32 roleMask = GetRoles();
    if (roleMask == lfg::PLAYER_ROLE_TANK)
        _roles = "TANK";
    else if (roleMask == lfg::PLAYER_ROLE_HEALER)
        _roles = "HEAL";
    else if (roleMask == lfg::PLAYER_ROLE_DAMAGE)
        _roles = "DPS";
    else if (roleMask == (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_DAMAGE))
        _roles = "TANK/DPS";

    RTG_ClearDungeonQueuePenalties(bot);

    if (RTG_IsQueuedLfgBot(bot))
    {
        RTG_RecordRuntimeRole(bot, roleMask);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_pending", 1, 20, sRandomPlayerbotMgr.RTG_GetBotEventData(botId, "add"));
        LOG_INFO("playerbots", "[RTG][RDF][JOIN] helper={} roleMask={} selectedCount={} grouped={}", botId, roleMask, static_cast<uint32>(list.size()), bot->GetGroup() ? 1u : 0u);
    }

    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: queues LFG, Dungeon as {} ({})", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), _roles,
             many ? "several dungeons" : dungeon->Name[0]);
    if (RTG_LfgDebugEnabled())
        LOG_INFO("playerbots", "[RTGDBG][LFGJOIN] bot={} roleMask={} selectedCount={} grouped={} queuedHelper={}", botId, roleMask, static_cast<uint32>(list.size()), bot->GetGroup() ? 1u : 0u, RTG_IsQueuedLfgBot(bot) ? 1u : 0u);

    // Set RbotAId Browser comment
    std::string const _gs = std::to_string(botAI->GetEquipGearScore(bot/*, false, false*/));

    // JoinLfg is not threadsafe, so make packet and queue into session
    // sLFGMgr->JoinLfg(bot, roleMask, list, _gs);

    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << (uint32)roleMask;
    *data << (bool)false;
    *data << (bool)false;
    // Slots
    *data << (uint8)(list.size());
    for (uint32 dungeonId : list)
        *data << (uint32)dungeonId;
    // Needs
    *data << (uint8)3 << (uint8)0 << (uint8)0 << (uint8)0;
    *data << _gs;
    bot->GetSession()->QueuePacket(data);

    // RTG retry timing:
    // - fresh queued fill bots with no group should retry quickly
    // - grouped/post-dungeon bots should retry more calmly
    // - normal bots keep the default pacing
    uint32 retryDelay = 8;
    if (RTG_IsQueuedLfgBot(bot))
    {
        if (bot->GetGroup())
            retryDelay = 10;
        else
            retryDelay = 2;
    }

    rtgNextJoinAttempt[botId] = now + retryDelay;

    return true;
}

bool LfgRoleCheckAction::Execute(Event event)
{
    if (RTG_GroupHasRealPlayerMember(bot) && !RTG_IsQueuedLfgBot(bot) && !RTG_IsAssignedLfgHelper(bot))
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGROLE] bot={} blocked real-player-group", bot->GetGUID().GetCounter());
        return false;
    }

    if (Group* group = bot->GetGroup())
    {
        uint32 currentRoles = sLFGMgr->GetRoles(bot->GetGUID());
        uint32 newRoles = GetRoles();
        // if (currentRoles == newRoles)
        //     return false;

        WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_ROLES);
        *packet << (uint8)newRoles;
        bot->GetSession()->QueuePacket(packet);
        // sLFGMgr->SetRoles(bot->GetGUID(), newRoles);
        // sLFGMgr->UpdateRoleCheck(group->GetGUID(), bot->GetGUID(), newRoles);

        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: LFG roles checked", bot->GetGUID().ToString().c_str(),
                 bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGROLE] bot={} currentRoles={} newRoles={} groupGuid={}", bot->GetGUID().GetCounter(), currentRoles, newRoles, group->GetGUID().GetCounter());

        return true;
    }

    return false;
}

bool LfgAcceptAction::Execute(Event event)
{
    uint32 botId = bot->GetGUID().GetCounter();
    bool queuedLfgBot = RTG_IsQueuedLfgBot(bot);
    uint32 id = AI_VALUE(uint32, "lfg proposal");

    auto clearStoredProposal = [&]()
    {
        botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
    };

    auto clearQueuedProposalLifecycle = [&]()
    {
        if (!queuedLfgBot)
            return;

        std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(botId, "add");
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_proposal_lock", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_accept_sent", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_teleport_sent", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_accept_proposal", 0, 0, addData);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_teleport_attempts", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_group_ready_since", 0, 0);
    };

    auto markQueuedProposalAccepted = [&](uint32 proposalId)
    {
        std::string addData = sRandomPlayerbotMgr.RTG_GetBotEventData(botId, "add");
        uint32 nowTs = uint32(time(nullptr));
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_proposal_lock", proposalId, 90, addData);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_accept_sent", nowTs, 90, addData);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_teleport_sent", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_accept_proposal", proposalId, 120, addData);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_teleport_attempts", 0, 0);
        sRandomPlayerbotMgr.RTG_SetBotEventValue(botId, "rtg_lfg_group_ready_since", 0, 0);
    };

    RTG_LfgProposalPacketData packetData;
    bool havePacket = !event.getPacket().empty() && RTG_ParseProposalPacket(event.getPacket(), packetData);
    if (havePacket)
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGPROPOSAL] bot={} dungeonEntry={} proposal={} state={} silent={} groupSize={} bytes={}",
                botId, packetData.dungeonEntry, packetData.proposalId, uint32(packetData.state),
                uint32(packetData.silent), uint32(packetData.groupSize), event.getPacket().size());

        if (packetData.state == uint8(lfg::LFG_PROPOSAL_INITIATING) && packetData.proposalId)
        {
            id = packetData.proposalId;
            botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(id);
        }
        else
        {
            clearStoredProposal();

            if (queuedLfgBot && packetData.state == uint8(lfg::LFG_PROPOSAL_FAILED))
                clearQueuedProposalLifecycle();

            return false;
        }
    }

    if (!id)
        return false;

    if (queuedLfgBot && RTG_GetAcceptedProposal(bot) == id)
    {
        if (RTG_LfgDebugEnabled())
            LOG_INFO("playerbots", "[RTGDBG][LFGPROPOSAL][SUPPRESS] bot={} proposal={} reason=already_accepted", botId, id);
        return false;
    }

    if (!queuedLfgBot && (bot->IsInCombat() || bot->isDead()))
    {
        WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
        *packet << id << false;
        bot->GetSession()->QueuePacket(packet);
        clearStoredProposal();
        return true;
    }

    if (queuedLfgBot)
    {
        bot->CombatStop(true);
        LOG_INFO("playerbots", "[RTG][RDF][ACCEPT] helper={} proposal={} source={}",
            botId, id, havePacket ? "packet" : "stored");
        markQueuedProposalAccepted(id);
    }

    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
    *packet << id << true;
    bot->GetSession()->QueuePacket(packet);

    if (queuedLfgBot)
        return true;

    clearStoredProposal();
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && !bot->GetGroup())
    {
        sRandomPlayerbotMgr.Refresh(bot);
        botAI->ResetStrategies();
    }

    botAI->Reset();
    return true;
}

bool LfgLeaveAction::Execute(Event event)
{
    if (sLFGMgr->GetState(bot->GetGUID()) > LFG_STATE_QUEUED)
        return false;

    if (RTG_IsQueuedLfgBot(bot))
        RTG_ClearDungeonQueuePenalties(bot);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
    bot->GetSession()->QueuePacket(packet);
    return true;
}

bool LfgLeaveAction::isUseful() { return true; }

bool LfgTeleportAction::Execute(Event event)
{
    bool out = false;

    WorldPacket p(event.getPacket());
    if (!p.empty() && p.size() >= sizeof(bool))
    {
        p.rpos(0);
        p >> out;
    }

    if (RTG_IsQueuedLfgBot(bot))
    {
        out = false;

        Group* group = bot->GetGroup();
        lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
        if (!group || !group->isLFGGroup())
        {
            LOG_INFO("playerbots", "[RTG][RDF][WAIT_GROUP] helper={} grouped={} state={}",
                     bot->GetGUID().GetCounter(), group ? 1 : 0, uint32(state));
            return false;
        }

        bot->CombatStop(true);
    }

    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_TELEPORT, 1);
    *packet << uint8(out ? 1 : 0);
    bot->GetSession()->QueuePacket(packet);

    if (RTG_IsQueuedLfgBot(bot))
        LOG_INFO("playerbots", "[RTG][RDF][TELEPORT] helper={} out={} grouped={} state={}",
                 bot->GetGUID().GetCounter(), out ? 1 : 0, bot->GetGroup() ? 1 : 0, uint32(sLFGMgr->GetState(bot->GetGUID())));

    return true;
}

bool LfgJoinAction::isUseful()
{
    if (!sPlayerbotAIConfig.randomBotJoinLfg)
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
    }

    if (bot->GetLevel() < 15)
        return false;

    // don't use if active player master
    if (GET_PLAYERBOT_AI(bot)->IsRealPlayer())
        return false;

    if (bot->GetGroup())
    {
        if (RTG_GroupHasRealPlayerMember(bot))
            return false;

        if (bot->GetGroup()->GetLeaderGUID() != bot->GetGUID())
        {
            // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
            return false;
        }
    }

    if (bot->IsBeingTeleported())
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (bot->isDead())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    Map* map = bot->GetMap();
    if (map && map->Instanceable())
        return false;

    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
        return false;

    return true;
}
