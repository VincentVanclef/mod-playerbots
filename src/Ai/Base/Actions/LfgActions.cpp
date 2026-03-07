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
#include "World.h"
#include "WorldPacket.h"

#include <unordered_map>

using namespace lfg;

namespace
{
    static bool RTG_HasPrefix(std::string const& value, std::string const& prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    static bool RTG_ParseLfgDesiredRole(std::string const& data, uint32& role)
    {
        role = 0;
        if (!RTG_HasPrefix(data, "rtg_lfg:"))
            return false;

        size_t first = data.find(':');
        size_t second = data.find(':', first + 1);
        size_t third = data.find(':', second == std::string::npos ? std::string::npos : second + 1);
        if (third == std::string::npos)
            return false;

        try
        {
            role = static_cast<uint32>(std::stoul(data.substr(third + 1)));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool RTG_ClassCanRole(uint8 cls, uint32 role)
    {
        switch (role)
        {
            case lfg::PLAYER_ROLE_TANK:
                return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_DEATH_KNIGHT;
            case lfg::PLAYER_ROLE_HEALER:
                return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
            case lfg::PLAYER_ROLE_DAMAGE:
                return true;
            default:
                return false;
        }
    }
	
	static void RTG_ApplyDungeonDeserter(Player* bot)
	{
		uint32 const spellId = 71041; // Dungeon Deserter
		if (bot && !bot->HasAura(spellId))
			bot->CastSpell(bot, spellId, true);
	}

    static uint32 RTG_ActualRoleForBot(Player* bot)
    {
        uint8 spec = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
            case CLASS_DRUID:
                if (spec == DRUID_TAB_RESTORATION)
                    return lfg::PLAYER_ROLE_HEALER;
                if (spec == DRUID_TAB_FERAL)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_PALADIN:
                if (spec == PALADIN_TAB_HOLY)
                    return lfg::PLAYER_ROLE_HEALER;
                if (spec == PALADIN_TAB_PROTECTION)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_PRIEST:
                if (spec == PRIEST_TAB_SHADOW)
                    return lfg::PLAYER_ROLE_DAMAGE;
                return lfg::PLAYER_ROLE_HEALER;
            case CLASS_SHAMAN:
                if (spec == SHAMAN_TAB_RESTORATION)
                    return lfg::PLAYER_ROLE_HEALER;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_WARRIOR:
                if (spec == WARRIOR_TAB_PROTECTION)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_DEATH_KNIGHT:
                if (spec == DEATH_KNIGHT_TAB_BLOOD)
                    return lfg::PLAYER_ROLE_TANK;
                return lfg::PLAYER_ROLE_DAMAGE;
            default:
                return lfg::PLAYER_ROLE_DAMAGE;
        }
    }

    static bool RTG_IsQueuedLfgBot(Player* bot)
    {
        return RTG_HasPrefix(sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add"), "rtg_lfg:");
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

    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 desiredRole = 0;
        if (RTG_ParseLfgDesiredRole(sRandomPlayerbotMgr.RTG_GetBotEventData(bot->GetGUID().GetCounter(), "add"), desiredRole) && desiredRole)
        {
            if (desiredRole != actualRole)
            {
                LOG_INFO("playerbots", "Bot {} {}:{} <{}>: RTG desired LFG role {} mismatched actual spec role {}, using actual role", bot->GetGUID().ToString().c_str(),
                         bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), desiredRole, actualRole);
            }
        }
    }

    return actualRole;
}

bool LfgJoinAction::JoinLFG()
{
    static std::unordered_map<uint32, time_t> rtgNextJoinAttempt;

    // check if already in lfg
    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
    {
        rtgNextJoinAttempt.erase(bot->GetGUID().GetCounter());
        return false;
    }

    time_t now = time(nullptr);
    uint32 botId = bot->GetGUID().GetCounter();

    auto attemptIt = rtgNextJoinAttempt.find(botId);
    if (attemptIt != rtgNextJoinAttempt.end() && now < attemptIt->second)
        return false;

    // ------------------------------------------------------------------
    // RTG: Event-driven LFG grace
    // Only let bots queue for LFG after real players have initiated LFG queueing
    // and the grace window has expired.
    // ------------------------------------------------------------------
    if (sPlayerbotAIConfig.rtgEventDriven)
    {
        uint32 start = sRandomPlayerbotMgr.RTG_GetGlobalEvent("rtg_lfg_start");
        if (!start)
            return false;

        if (now < (time_t)(start + sPlayerbotAIConfig.rtgQueueGraceSeconds))
            return false;
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
    if (roleMask & lfg::PLAYER_ROLE_TANK)
        _roles = "TANK";

    if (roleMask & lfg::PLAYER_ROLE_HEALER)
        _roles = "HEAL";

    if (roleMask & lfg::PLAYER_ROLE_DAMAGE)
        _roles = "DPS";

    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: queues LFG, Dungeon as {} ({})", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), _roles,
             many ? "several dungeons" : dungeon->Name[0]);

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

        return true;
    }

    return false;
}

bool LfgAcceptAction::Execute(Event event)
{
    uint32 id = AI_VALUE(uint32, "lfg proposal");

    // Try accept if already stored
    if (id)
    {
        if (!RTG_IsQueuedLfgBot(bot) && (bot->IsInCombat() || bot->isDead()))
        {
            WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
            *packet << id << false;
            bot->GetSession()->QueuePacket(packet);
            return true;
        }
        if (RTG_IsQueuedLfgBot(bot))
            bot->CombatStop(true);

        botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
        bot->ClearUnitState(UNIT_STATE_ALL_STATE);

        WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
        *packet << id << true;
        bot->GetSession()->QueuePacket(packet);

        if (sRandomPlayerbotMgr.IsRandomBot(bot) && !bot->GetGroup())
        {
            sRandomPlayerbotMgr.Refresh(bot);
            botAI->ResetStrategies();
        }

        botAI->Reset();
        return true;
    }

    // If we get the proposal packet, accept immediately
    if (!event.getPacket().empty())
    {
        WorldPacket p(event.getPacket());
        uint32 dungeonId;
        uint8 state;
        p >> dungeonId >> state >> id;

        if (id)
        {
            if (!RTG_IsQueuedLfgBot(bot) && (bot->IsInCombat() || bot->isDead()))
            {
                WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
                *packet << id << false;
                bot->GetSession()->QueuePacket(packet);
                return true;
            }
            if (RTG_IsQueuedLfgBot(bot))
                bot->CombatStop(true);

            botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
            bot->ClearUnitState(UNIT_STATE_ALL_STATE);

            WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
            *packet << id << true;
            bot->GetSession()->QueuePacket(packet);

            if (sRandomPlayerbotMgr.IsRandomBot(bot) && !bot->GetGroup())
            {
                sRandomPlayerbotMgr.Refresh(bot);
                botAI->ResetStrategies();
            }

            botAI->Reset();
            return true;
        }
    }

    return false;
}

bool LfgLeaveAction::Execute(Event event)
{
    if (sLFGMgr->GetState(bot->GetGUID()) > LFG_STATE_QUEUED)
        return false;

    if (RTG_IsQueuedLfgBot(bot))
        RTG_ApplyDungeonDeserter(bot);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
    bot->GetSession()->QueuePacket(packet);
    return true;
}

bool LfgLeaveAction::isUseful() { return true; }

bool LfgTeleportAction::Execute(Event event)
{
    bool out = false;

    WorldPacket p(event.getPacket());
    if (!p.empty())
    {
        p.rpos(0);
        p >> out;
    }

    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_TELEPORT);
    *packet << out;
    bot->GetSession()->QueuePacket(packet);
    // sLFGMgr->TeleportPlayer(bot, out);

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

    if (bot->GetGroup() && bot->GetGroup()->GetLeaderGUID() != bot->GetGUID())
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
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
