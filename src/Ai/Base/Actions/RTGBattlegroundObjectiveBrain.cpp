/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RTGBattlegroundObjectiveBrain.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

#include "AiObjectContext.h"
#include "Battleground.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "GameObject.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Unit.h"
#include "Util.h"

namespace
{
constexpr uint32 RTG_BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 RTG_BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 RTG_BG_EY_NETHERSTORM_FLAG_SPELL = 34976;

constexpr uint32 RTG_OBJECTIVE_WSG_RETURN_OWN_FLAG = 1;
constexpr uint32 RTG_OBJECTIVE_WSG_CAPTURE_ENEMY_FLAG = 2;
constexpr uint32 RTG_OBJECTIVE_WSG_DEFEND_OWN_FLAG = 3;
constexpr uint32 RTG_OBJECTIVE_WSG_PRESSURE_ENEMY = 5;
constexpr uint32 RTG_OBJECTIVE_WSG_HUNT_OWN_FLAG = 6;

PositionInfo const WSG_HORDE_FLAG_ROOM(917.36304f, 1434.0795f, 346.34628f, 489);
PositionInfo const WSG_ALLIANCE_FLAG_ROOM(1540.2811f, 1481.4397f, 352.63336f, 489);
PositionInfo const WSG_MIDFIELD(1227.446f, 1476.235f, 307.484f, 489);
PositionInfo const WSG_HORDE_BASE_EXIT(1016.42f, 1402.33f, 341.352f, 489);
PositionInfo const WSG_ALLIANCE_BASE_EXIT(1443.55f, 1533.40f, 343.148f, 489);

PositionInfo const EOTS_CENTER(2175.0f, 1569.0f, 1159.0f, 566);
PositionInfo const EOTS_HORDE_START(1809.102f, 1540.854f, 1267.142f, 566);
PositionInfo const EOTS_ALLIANCE_START(2523.827f, 1596.915f, 1270.204f, 566);
PositionInfo const EOTS_HORDE_ROAD_FORK(1941.452f, 1549.086f, 1176.700f, 566);
PositionInfo const EOTS_ALLIANCE_ROAD_FORK(2395.737f, 1588.287f, 1176.570f, 566);
constexpr float RTG_EOTS_START_EXIT_MIN_Z = 1218.0f;

struct RTGTowerDef
{
    uint32 nodeId;
    char const* name;
    PositionInfo capture;
    PositionInfo allianceApproach;
    PositionInfo hordeApproach;
    PositionInfo defense;
    PositionInfo fallback;
};

RTGTowerDef const EOTS_TOWERS[] = {
    {POINT_FEL_REAVER, "FRR",
     PositionInfo(2043.8687f, 1730.0178f, 1189.8501f, 566),
     PositionInfo(2132.0f, 1705.0f, 1178.0f, 566),
     PositionInfo(1996.0f, 1666.0f, 1182.0f, 566),
     PositionInfo(2043.8687f, 1730.0178f, 1189.8501f, 566),
     EOTS_HORDE_ROAD_FORK},
    {POINT_BLOOD_ELF, "BET",
     PositionInfo(2048.3354f, 1392.6724f, 1194.3562f, 566),
     PositionInfo(2132.0f, 1436.0f, 1178.0f, 566),
     PositionInfo(1994.0f, 1466.0f, 1180.0f, 566),
     PositionInfo(2048.3354f, 1392.6724f, 1194.3562f, 566),
     EOTS_HORDE_ROAD_FORK},
    {POINT_DRAENEI_RUINS, "DR",
     PositionInfo(2286.697f, 1402.5239f, 1197.133f, 566),
     PositionInfo(2355.0f, 1461.0f, 1180.0f, 566),
     PositionInfo(2215.0f, 1432.0f, 1178.0f, 566),
     PositionInfo(2286.697f, 1402.5239f, 1197.133f, 566),
     EOTS_ALLIANCE_ROAD_FORK},
    {POINT_MAGE_TOWER, "MT",
     PositionInfo(2284.7944f, 1731.2412f, 1189.8682f, 566),
     PositionInfo(2357.0f, 1688.0f, 1178.0f, 566),
     PositionInfo(2216.0f, 1699.0f, 1178.0f, 566),
     PositionInfo(2284.7944f, 1731.2412f, 1189.8682f, 566),
     EOTS_ALLIANCE_ROAD_FORK},
};

struct RTGObjectiveBrainState
{
    RTGBgObjectiveAssignment assignment;
    uint32 bgInstance = 0;
    uint32 bgType = 0;
    TeamId team = TEAM_NEUTRAL;
    PositionInfo trackedObjective;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastZ = 0.0f;
    uint32 lastProgressMs = 0;
    uint32 stuckSinceMs = 0;
    bool progressInitialized = false;
};

std::unordered_map<uint64, RTGObjectiveBrainState> RTGObjectiveStates;

struct RTGTeamSizeCacheEntry
{
    uint32 sampledAtMs = 0;
    uint32 count = 0;
};

std::unordered_map<uint64, RTGTeamSizeCacheEntry> RTGTeamSizeCache;
constexpr uint32 RTG_TEAM_SIZE_CACHE_MS = 250;

BattlegroundTypeId RTG_GetRealBgType(Battleground* bg)
{
    if (!bg)
        return BATTLEGROUND_TYPE_NONE;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    return bgType;
}

uint64 RTG_StateKey(Player* bot, Battleground* bg)
{
    if (!bot || !bg)
        return 0;

    return (uint64(bg->GetInstanceID()) << 32) ^ uint64(bot->GetGUID().GetCounter());
}

char const* RTG_MapName(BattlegroundTypeId bgType)
{
    switch (bgType)
    {
        case BATTLEGROUND_WS:
            return "WSG";
        case BATTLEGROUND_EY:
            return "EOTS";
        default:
            return "BG";
    }
}

char const* RTG_TeamName(TeamId team)
{
    if (team == TEAM_ALLIANCE)
        return "Alliance";
    if (team == TEAM_HORDE)
        return "Horde";
    return "Neutral";
}

void RTG_Debug(Player* bot, Battleground* bg, RTGBgObjectiveAssignment const& assignment, char const* phase,
               char const* extra = nullptr)
{
    if (!sPlayerbotAIConfig.rtgBgDebugObjectiveBrain || !bot || !bg)
        return;

    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);
    TeamId team = RTG_GetEffectiveBgTeam(bot);
    LOG_INFO("playerbots",
             "[RTG-BG] bot={} map={} team={} phase={} role={} dest={:.2f},{:.2f},{:.2f} objective={} emergency={} reason={}{}{} commit={}",
             bot->GetName(), RTG_MapName(bgType), RTG_TeamName(team), phase ? phase : "?",
             RTG_BgObjectiveRoleName(assignment.role), assignment.destination.x, assignment.destination.y,
             assignment.destination.z, assignment.objectiveId, assignment.emergency, assignment.reason.c_str(),
             extra ? " " : "", extra ? extra : "", assignment.minCommitMs);
}

void RTG_SetDestination(RTGBgObjectiveAssignment& assignment, PositionInfo const& pos, uint32 mapId)
{
    assignment.destination.Set(pos.x, pos.y, pos.z, mapId);
}

float RTG_Distance2d(Player* bot, PositionInfo const& pos)
{
    return bot ? bot->GetExactDist2d(pos.x, pos.y) : FLT_MAX;
}

float RTG_Distance3d(Player* bot, PositionInfo const& pos)
{
    return bot ? bot->GetExactDist(pos.x, pos.y, pos.z) : FLT_MAX;
}

bool RTG_UnitValidForObjective(Player* bot, Unit* unit)
{
    return bot && unit && unit->IsInWorld() && unit->IsAlive() && unit->GetMapId() == bot->GetMapId();
}

bool RTG_TargetAssignmentStillValid(PlayerbotAI* botAI, Player* bot, ObjectGuid const& targetGuid)
{
    if (!botAI || targetGuid.IsEmpty())
        return false;

    return RTG_UnitValidForObjective(bot, botAI->GetUnit(targetGuid));
}

AiObjectContext* RTG_Context(PlayerbotAI* botAI)
{
    return botAI ? botAI->GetAiObjectContext() : nullptr;
}

template <class T>
T RTG_GetValue(PlayerbotAI* botAI, std::string const& name, T fallback = T())
{
    AiObjectContext* context = RTG_Context(botAI);
    if (!context)
        return fallback;

    return context->GetValue<T>(name)->Get();
}

Unit* RTG_GetUnitValue(PlayerbotAI* botAI, std::string const& name)
{
    AiObjectContext* context = RTG_Context(botAI);
    if (!context)
        return nullptr;

    return context->GetValue<Unit*>(name)->Get();
}

uint32 RTG_StableRole(PlayerbotAI* botAI, Player* bot)
{
    uint32 role = RTG_GetValue<uint32>(botAI, "bg role", 0);
    if (!bot)
        return role % 10;

    return (role + (bot->GetGUID().GetCounter() % 10)) % 10;
}

bool RTG_IsWsgFlagCarrier(Player* bot, BattlegroundWS* wsBg, TeamId team)
{
    if (!bot || !wsBg || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return false;

    if (bot->HasAura(RTG_BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(RTG_BG_WS_SPELL_SILVERWING_FLAG))
        return true;

    TeamId const enemyTeam = wsBg->GetOtherTeamId(team);
    return wsBg->GetFlagPickerGUID(enemyTeam) == bot->GetGUID();
}

bool RTG_IsEotsFlagCarrier(Player* bot, BattlegroundEY* eyeBg)
{
    if (!bot || !eyeBg)
        return false;

    return bot->HasAura(RTG_BG_EY_NETHERSTORM_FLAG_SPELL) || eyeBg->GetFlagPickerGUID() == bot->GetGUID();
}

uint32 RTG_ClassFlagPickupRank(Player* bot)
{
    if (!bot)
        return 10;

    switch (bot->getClass())
    {
        case CLASS_DRUID:
            return 0;
        case CLASS_HUNTER:
            return 1;
        case CLASS_ROGUE:
            return 2;
        case CLASS_SHAMAN:
            return 3;
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
            return 4;
        default:
            return 6;
    }
}

uint32 RTG_TeamSize(Battleground* bg, TeamId team)
{
    if (!bg || !bg->GetBgMap())
        return 0;

    uint32 const now = getMSTime();
    uint64 const cacheKey = (uint64(bg->GetInstanceID()) << 8) | uint64(uint32(team) & 0xFFu);
    auto cacheItr = RTGTeamSizeCache.find(cacheKey);
    if (cacheItr != RTGTeamSizeCache.end() &&
        getMSTimeDiff(cacheItr->second.sampledAtMs, now) < RTG_TEAM_SIZE_CACHE_MS)
    {
        return cacheItr->second.count;
    }

    uint32 count = 0;
    for (auto& ref : bg->GetBgMap()->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (player && RTG_GetEffectiveBgTeam(player) == team)
            ++count;
    }

    RTGTeamSizeCache[cacheKey] = { now, count };

    // Avoid retaining instance ids forever on long uptimes. This runs only after
    // the small cache has grown beyond normal active-BG usage.
    if (RTGTeamSizeCache.size() > 64)
    {
        for (auto itr = RTGTeamSizeCache.begin(); itr != RTGTeamSizeCache.end();)
        {
            if (getMSTimeDiff(itr->second.sampledAtMs, now) > 5000u)
                itr = RTGTeamSizeCache.erase(itr);
            else
                ++itr;
        }
    }

    return count;
}

bool RTG_NearestGameObject(PlayerbotAI* botAI, Player* bot, uint32 entry, float maxDistance,
                           PositionInfo& outPos, ObjectGuid& outGuid)
{
    if (!botAI || !bot)
        return false;

    AiObjectContext* context = RTG_Context(botAI);
    if (!context)
        return false;

    GuidVector objects = context->GetValue<GuidVector>("nearest game objects no los")->Get();
    if (objects.empty())
        objects = context->GetValue<GuidVector>("nearest game objects")->Get();

    float bestDist = maxDistance;
    bool found = false;
    for (ObjectGuid const& guid : objects)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || go->GetEntry() != entry || !go->isSpawned())
            continue;

        float const dist = bot->GetDistance(go);
        if (dist > bestDist)
            continue;

        bestDist = dist;
        outGuid = go->GetGUID();
        outPos.Set(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), go->GetMapId());
        found = true;
    }

    return found;
}

PositionInfo const& RTG_WsgOwnFlagRoom(TeamId team)
{
    return team == TEAM_ALLIANCE ? WSG_ALLIANCE_FLAG_ROOM : WSG_HORDE_FLAG_ROOM;
}

PositionInfo const& RTG_WsgEnemyFlagRoom(TeamId team)
{
    return team == TEAM_ALLIANCE ? WSG_HORDE_FLAG_ROOM : WSG_ALLIANCE_FLAG_ROOM;
}

PositionInfo RTG_WsgEnemyFlagDestination(BattlegroundWS* wsBg, TeamId team)
{
    PositionInfo target = RTG_WsgEnemyFlagRoom(team);
    if (!wsBg || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return target;

    TeamId const enemyTeam = wsBg->GetOtherTeamId(team);
    uint32 const objectId = enemyTeam == TEAM_HORDE ? BG_WS_OBJECT_H_FLAG : BG_WS_OBJECT_A_FLAG;
    if (GameObject* flag = wsBg->GetBGObject(objectId))
    {
        if (flag->isSpawned())
            target.Set(flag->GetPositionX(), flag->GetPositionY(), flag->GetPositionZ(), flag->GetMapId());
    }

    return target;
}

PositionInfo RTG_WsgBaseExit(TeamId team)
{
    return team == TEAM_ALLIANCE ? WSG_ALLIANCE_BASE_EXIT : WSG_HORDE_BASE_EXIT;
}

PositionInfo RTG_WsgEnemyCarrierSearchDestination(TeamId team)
{
    return team == TEAM_ALLIANCE ? WSG_HORDE_BASE_EXIT : WSG_ALLIANCE_BASE_EXIT;
}

bool RTG_SelectWsgObjective(PlayerbotAI* botAI, Player* bot, Battleground* bg, RTGBgObjectiveAssignment& out)
{
    BattlegroundWS* wsBg = dynamic_cast<BattlegroundWS*>(bg);
    if (!botAI || !bot || !wsBg)
        return false;

    TeamId const team = RTG_GetEffectiveBgTeam(bot);
    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return false;

    TeamId const enemyTeam = wsBg->GetOtherTeamId(team);
    uint32 const stableRole = RTG_StableRole(botAI, bot);
    uint32 const teamSize = std::max<uint32>(1, RTG_TeamSize(bg, team));
    bool const smallTeam = teamSize <= 6;
    uint32 const pickupRank = RTG_ClassFlagPickupRank(bot);
    bool const pickupRole = smallTeam ? (stableRole % 5 == 0 || pickupRank <= 1)
                                      : (stableRole == 0 || stableRole == 1 || (pickupRank <= 2 && stableRole < 6));
    bool const escortRole = smallTeam ? (stableRole % 5 == 1) : (stableRole == 2);
    bool const hunterRole = smallTeam ? (stableRole % 5 == 2 || stableRole % 5 == 3)
                                      : (stableRole == 3 || stableRole == 4 || stableRole == 5);
    bool const defenderRole = smallTeam ? (stableRole % 5 == 4) : (stableRole == 8);

    out.valid = true;
    out.assignedAtMs = getMSTime();
    out.minCommitMs = sPlayerbotAIConfig.rtgBgWsgCommitMs;

    if (RTG_IsWsgFlagCarrier(bot, wsBg, team))
    {
        bool const ownFlagTaken = !wsBg->GetFlagPickerGUID(team).IsEmpty();
        out.role = ownFlagTaken ? RTGBgObjectiveRole::DefendObjective : RTGBgObjectiveRole::CaptureEnemyFlag;
        out.objectiveId = ownFlagTaken ? RTG_OBJECTIVE_WSG_DEFEND_OWN_FLAG : RTG_OBJECTIVE_WSG_CAPTURE_ENEMY_FLAG;
        RTG_SetDestination(out, RTG_WsgOwnFlagRoom(team), bg->GetMapId());
        out.emergency = true;
        out.reason = ownFlagTaken ? "CarrierHideOwnFlagMissing" : "CarrierCapOwnFlagHome";
        return true;
    }

    PositionInfo droppedFlag;
    ObjectGuid droppedGuid;
    uint32 const ownDroppedEntry = team == TEAM_ALLIANCE ? BG_OBJECT_A_FLAG_GROUND_WS_ENTRY
                                                        : BG_OBJECT_H_FLAG_GROUND_WS_ENTRY;
    if (wsBg->GetFlagState(team) != BG_WS_FLAG_STATE_ON_BASE &&
        wsBg->GetFlagState(team) != BG_WS_FLAG_STATE_ON_PLAYER &&
        RTG_NearestGameObject(botAI, bot, ownDroppedEntry, 85.0f, droppedFlag, droppedGuid))
    {
        out.role = RTGBgObjectiveRole::ReturnOwnFlag;
        out.objectiveId = RTG_OBJECTIVE_WSG_RETURN_OWN_FLAG;
        out.targetGuid = droppedGuid;
        RTG_SetDestination(out, droppedFlag, bg->GetMapId());
        out.minCommitMs = 6000;
        out.emergency = true;
        out.reason = "OwnFlagDroppedNearby";
        return true;
    }

    Unit* enemyFC = RTG_GetUnitValue(botAI, "enemy flag carrier");
    Unit* friendlyFC = RTG_GetUnitValue(botAI, "team flag carrier");
    Unit* enemyPlayer = RTG_GetUnitValue(botAI, "enemy player target");
    bool const ownFlagTaken = wsBg->GetFlagState(team) == BG_WS_FLAG_STATE_ON_PLAYER;
    bool const enemyFlagAtBase = wsBg->GetFlagState(enemyTeam) == BG_WS_FLAG_STATE_ON_BASE;

    if (RTG_UnitValidForObjective(bot, enemyFC) && (hunterRole || bot->GetDistance(enemyFC) < 155.0f))
    {
        out.role = RTGBgObjectiveRole::KillEnemyFlagCarrier;
        out.objectiveId = uint32(enemyFC->GetGUID().GetCounter());
        out.targetGuid = enemyFC->GetGUID();
        out.destination.Set(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ(), bg->GetMapId());
        out.emergency = bot->GetDistance(enemyFC) < 90.0f;
        out.reason = out.emergency ? "EnemyFlagCarrierNearby" : "EnemyFlagCarrierKnown";
        return true;
    }

    bool friendlyCarrierThreatened = RTG_UnitValidForObjective(bot, friendlyFC) &&
        RTG_UnitValidForObjective(bot, enemyPlayer) && enemyPlayer->GetDistance(friendlyFC) < 40.0f;
    if (RTG_UnitValidForObjective(bot, friendlyFC) && (escortRole || (friendlyCarrierThreatened && bot->GetDistance(friendlyFC) < 70.0f)))
    {
        out.role = RTGBgObjectiveRole::EscortFriendlyFlagCarrier;
        out.objectiveId = uint32(friendlyFC->GetGUID().GetCounter());
        out.targetGuid = friendlyFC->GetGUID();
        out.destination.Set(friendlyFC->GetPositionX(), friendlyFC->GetPositionY(), friendlyFC->GetPositionZ(), bg->GetMapId());
        out.emergency = friendlyCarrierThreatened;
        out.reason = friendlyCarrierThreatened ? "FriendlyCarrierThreatened" : "AssignedFlagCarrierEscort";
        return true;
    }

    if (RTG_UnitValidForObjective(bot, enemyPlayer) &&
        (!defenderRole || ownFlagTaken || bot->GetDistance(enemyPlayer) < 95.0f))
    {
        out.role = RTGBgObjectiveRole::MidfieldPressure;
        out.objectiveId = RTG_OBJECTIVE_WSG_PRESSURE_ENEMY;
        out.targetGuid = enemyPlayer->GetGUID();
        out.destination.Set(enemyPlayer->GetPositionX(), enemyPlayer->GetPositionY(), enemyPlayer->GetPositionZ(), bg->GetMapId());
        out.minCommitMs = 6000;
        out.emergency = ownFlagTaken || bot->GetDistance(enemyPlayer) < 70.0f;
        out.reason = out.emergency ? "EnemyPressureNearby" : "EnemyPressureVisible";
        return true;
    }

    if (ownFlagTaken && !RTG_UnitValidForObjective(bot, enemyFC))
    {
        out.role = RTGBgObjectiveRole::MidfieldPressure;
        out.objectiveId = RTG_OBJECTIVE_WSG_HUNT_OWN_FLAG;
        RTG_SetDestination(out, RTG_WsgEnemyCarrierSearchDestination(team), bg->GetMapId());
        out.minCommitMs = 8000;
        out.emergency = true;
        out.reason = "HuntMissingEnemyCarrier";
        return true;
    }

    if (enemyFlagAtBase && pickupRole)
    {
        out.role = RTGBgObjectiveRole::CaptureEnemyFlag;
        out.objectiveId = RTG_OBJECTIVE_WSG_CAPTURE_ENEMY_FLAG;
        RTG_SetDestination(out, RTG_WsgEnemyFlagDestination(wsBg, team), bg->GetMapId());
        out.reason = "EnemyFlagHome";
        return true;
    }

    if (defenderRole)
    {
        out.role = RTGBgObjectiveRole::DefendObjective;
        out.objectiveId = RTG_OBJECTIVE_WSG_DEFEND_OWN_FLAG;
        RTG_SetDestination(out, RTG_WsgOwnFlagRoom(team), bg->GetMapId());
        out.reason = "AssignedHomeDefense";
        return true;
    }

    if (stableRole == 6 || stableRole == 7 || (!enemyFlagAtBase && !pickupRole))
    {
        if (!enemyFlagAtBase)
        {
            out.role = RTGBgObjectiveRole::MidfieldPressure;
            out.objectiveId = RTG_OBJECTIVE_WSG_HUNT_OWN_FLAG;
            RTG_SetDestination(out, RTG_WsgEnemyCarrierSearchDestination(team), bg->GetMapId());
            out.minCommitMs = 8000;
            out.reason = "RoamEnemyFlagSide";
            return true;
        }

        out.role = RTGBgObjectiveRole::CaptureEnemyFlag;
        out.objectiveId = RTG_OBJECTIVE_WSG_CAPTURE_ENEMY_FLAG;
        RTG_SetDestination(out, RTG_WsgEnemyFlagDestination(wsBg, team), bg->GetMapId());
        out.reason = "MidfieldPushEnemyFlag";
        return true;
    }

    out.role = RTGBgObjectiveRole::CaptureEnemyFlag;
    out.objectiveId = RTG_OBJECTIVE_WSG_CAPTURE_ENEMY_FLAG;
    RTG_SetDestination(out, RTG_WsgEnemyFlagDestination(wsBg, team), bg->GetMapId());
    out.reason = "DefaultEnemyFlagPressure";
    return true;
}

RTGTowerDef const* RTG_EotsTower(uint32 nodeId)
{
    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (tower.nodeId == nodeId)
            return &tower;

    return nullptr;
}

bool RTG_EotsNodeOwned(BattlegroundEY* eyeBg, uint32 nodeId, TeamId team)
{
    return eyeBg && nodeId && eyeBg->GetCapturePointInfo(nodeId)._ownerTeamId == team;
}

bool RTG_EotsIsHomeNode(TeamId team, uint32 nodeId)
{
    if (team == TEAM_HORDE)
        return nodeId == POINT_FEL_REAVER || nodeId == POINT_BLOOD_ELF;

    return nodeId == POINT_MAGE_TOWER || nodeId == POINT_DRAENEI_RUINS;
}

bool RTG_EotsSplitSecond(uint32 stableRole, uint32 seed, uint32 salt)
{
    uint32 const hash = seed * 1103515245u + stableRole * 12345u + salt;
    return ((hash >> 16) & 1u) != 0u;
}

uint32 RTG_EotsOwnedCount(BattlegroundEY* eyeBg, TeamId team)
{
    uint32 count = 0;
    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            ++count;

    return count;
}

uint32 RTG_EotsHomeOwnedCount(BattlegroundEY* eyeBg, TeamId team)
{
    uint32 count = 0;
    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (RTG_EotsIsHomeNode(team, tower.nodeId) && RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            ++count;

    return count;
}

uint32 RTG_EotsAssignedHomeNode(BattlegroundEY* eyeBg, TeamId team, uint32 stableRole, uint32 seed)
{
    if (team == TEAM_HORDE)
    {
        bool const needFelReaver = !RTG_EotsNodeOwned(eyeBg, POINT_FEL_REAVER, team);
        bool const needBloodElf = !RTG_EotsNodeOwned(eyeBg, POINT_BLOOD_ELF, team);
        if (needFelReaver && needBloodElf)
            return RTG_EotsSplitSecond(stableRole, seed, 0xE0750001u) ? POINT_BLOOD_ELF : POINT_FEL_REAVER;
        if (needFelReaver)
            return POINT_FEL_REAVER;
        if (needBloodElf)
            return POINT_BLOOD_ELF;
        return RTG_EotsSplitSecond(stableRole, seed, 0xE0750002u) ? POINT_BLOOD_ELF : POINT_FEL_REAVER;
    }

    if (team == TEAM_ALLIANCE)
    {
        bool const needMageTower = !RTG_EotsNodeOwned(eyeBg, POINT_MAGE_TOWER, team);
        bool const needDraeneiRuins = !RTG_EotsNodeOwned(eyeBg, POINT_DRAENEI_RUINS, team);
        if (needMageTower && needDraeneiRuins)
            return RTG_EotsSplitSecond(stableRole, seed, 0xE0750003u) ? POINT_DRAENEI_RUINS : POINT_MAGE_TOWER;
        if (needMageTower)
            return POINT_MAGE_TOWER;
        if (needDraeneiRuins)
            return POINT_DRAENEI_RUINS;
        return RTG_EotsSplitSecond(stableRole, seed, 0xE0750004u) ? POINT_DRAENEI_RUINS : POINT_MAGE_TOWER;
    }

    return 0;
}

uint32 RTG_EotsEnemyPressureNodeBySlot(TeamId team, uint32 slot)
{
    if (team == TEAM_HORDE)
        return (slot % 2u) == 0u ? POINT_MAGE_TOWER : POINT_DRAENEI_RUINS;

    if (team == TEAM_ALLIANCE)
        return (slot % 2u) == 0u ? POINT_BLOOD_ELF : POINT_FEL_REAVER;

    return 0;
}

bool RTG_EotsShouldPressureEnemyWithOneHome(uint32 stableRole, uint32 seed, uint32 teamSize)
{
    uint32 const slots = teamSize <= 6 ? 2u : (teamSize <= 10 ? 3u : 4u);
    uint32 const ticket = (seed * 1103515245u + stableRole * 12345u + 0xE0750007u) % 10u;
    return ticket < slots;
}

uint32 RTG_EotsAssignedEnemyNode(BattlegroundEY* eyeBg, TeamId team, uint32 stableRole, uint32 seed)
{
    uint32 const pressureSlot = stableRole + ((seed >> 4) & 1u);
    uint32 const first = RTG_EotsEnemyPressureNodeBySlot(team, pressureSlot);
    uint32 const second = RTG_EotsEnemyPressureNodeBySlot(team, pressureSlot + 1u);

    if (first && !RTG_EotsNodeOwned(eyeBg, first, team))
        return first;

    if (second && !RTG_EotsNodeOwned(eyeBg, second, team))
        return second;

    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (!RTG_EotsIsHomeNode(team, tower.nodeId) && !RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            return tower.nodeId;

    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (!RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            return tower.nodeId;

    return 0;
}

uint32 RTG_EotsAssignedDefenseNode(BattlegroundEY* eyeBg, TeamId team, uint32 stableRole, uint32 seed)
{
    uint32 assigned = RTG_EotsSplitSecond(stableRole, seed, 0xE0750006u)
        ? (team == TEAM_HORDE ? POINT_BLOOD_ELF : POINT_DRAENEI_RUINS)
        : (team == TEAM_HORDE ? POINT_FEL_REAVER : POINT_MAGE_TOWER);

    if (RTG_EotsNodeOwned(eyeBg, assigned, team))
        return assigned;

    for (RTGTowerDef const& tower : EOTS_TOWERS)
        if (RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            return tower.nodeId;

    return 0;
}

PositionInfo RTG_EotsTowerDestination(Player* bot, TeamId team, RTGTowerDef const& tower)
{
    PositionInfo const& fork = team == TEAM_HORDE ? EOTS_HORDE_ROAD_FORK : EOTS_ALLIANCE_ROAD_FORK;
    PositionInfo const& approach = team == TEAM_HORDE ? tower.hordeApproach : tower.allianceApproach;

    if (!bot)
        return tower.capture;

    if (tower.nodeId == POINT_FEL_REAVER)
    {
        if (RTG_Distance2d(bot, fork) > 95.0f && RTG_Distance2d(bot, tower.capture) > 135.0f)
            return fork;
        if (RTG_Distance2d(bot, approach) > 45.0f && RTG_Distance2d(bot, tower.capture) > 55.0f)
            return approach;
        return tower.capture;
    }

    if (RTG_Distance2d(bot, approach) > 65.0f && RTG_Distance2d(bot, tower.capture) > 70.0f)
        return approach;

    return tower.capture;
}

bool RTG_EotsCenterFlagSpawned(Battleground* bg, PositionInfo& out)
{
    out = EOTS_CENTER;
    if (!bg)
        return false;

    if (GameObject* flag = bg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM))
    {
        if (flag->isSpawned())
        {
            out.Set(flag->GetPositionX(), flag->GetPositionY(), flag->GetPositionZ(), flag->GetMapId());
            return true;
        }
    }

    return false;
}

bool RTG_EotsNearestOwnedTower(Player* bot, BattlegroundEY* eyeBg, TeamId team, uint32& nodeId, PositionInfo& pos)
{
    nodeId = 0;
    float bestDist = FLT_MAX;
    for (RTGTowerDef const& tower : EOTS_TOWERS)
    {
        if (!RTG_EotsNodeOwned(eyeBg, tower.nodeId, team))
            continue;

        float const dist = RTG_Distance3d(bot, tower.capture);
        if (dist < bestDist)
        {
            bestDist = dist;
            nodeId = tower.nodeId;
            pos = tower.capture;
        }
    }

    return nodeId != 0;
}

bool RTG_EotsShouldRunCenterFlag(Player* bot, uint32 stableRole, uint32 seed, uint32 ownedCount,
                                 uint32 homeOwnedCount, bool centerSpawned, uint32 teamSize)
{
    if (!centerSpawned || !bot)
        return false;

    uint32 slots = 0;
    if (ownedCount >= 3)
        slots = teamSize <= 6 ? 3 : 5;
    else if (ownedCount >= 2)
        slots = teamSize <= 6 ? 2 : 4;
    else if (homeOwnedCount >= 1)
        slots = 1;

    if (!slots)
        return false;

    uint32 const pickupRank = RTG_ClassFlagPickupRank(bot);
    uint32 const ticket = (seed * 1103515245u + stableRole * 12345u + 0xE07500F1u) % 10u;
    if (ticket < slots)
        return true;

    if (pickupRank <= 1 && ownedCount >= 2)
        return ((seed * 1103515245u + stableRole * 12345u + 0xE07500F2u) % 2u) == 0u;

    return pickupRank <= 1 && homeOwnedCount >= 1 &&
           ((seed * 1103515245u + stableRole * 12345u + 0xE07500F2u) % 4u) == 0u;
}

bool RTG_EotsShouldDefendTower(uint32 stableRole, uint32 ownedCount, uint32 teamSize)
{
    if (ownedCount < 3)
        return false;

    if (teamSize <= 6)
        return (stableRole % 6u) == 0u;

    if (teamSize <= 10)
        return (stableRole % 5u) == 0u;

    return (stableRole % 4u) == 0u;
}

bool RTG_EotsReachedStartExitField(Player* bot, TeamId team)
{
    if (!bot)
        return false;

    if (team == TEAM_HORDE)
        return bot->GetPositionX() >= 1932.0f;

    if (team == TEAM_ALLIANCE)
        return bot->GetPositionX() <= 2408.0f;

    return false;
}

bool RTG_EotsNeedsStartExit(Player* bot, TeamId team)
{
    if (!bot || bot->GetMapId() != 566)
        return false;

    if (RTG_EotsReachedStartExitField(bot, team))
        return false;

    if (bot->GetPositionZ() < RTG_EOTS_START_EXIT_MIN_Z)
        return false;

    PositionInfo const& start = team == TEAM_HORDE ? EOTS_HORDE_START : EOTS_ALLIANCE_START;
    return RTG_Distance3d(bot, start) <= 305.0f;
}

PositionInfo const& RTG_EotsStartExitDestination(TeamId team)
{
    return team == TEAM_HORDE ? EOTS_HORDE_ROAD_FORK : EOTS_ALLIANCE_ROAD_FORK;
}

bool RTG_SelectEotsObjective(PlayerbotAI* botAI, Player* bot, Battleground* bg, RTGBgObjectiveAssignment& out)
{
    BattlegroundEY* eyeBg = dynamic_cast<BattlegroundEY*>(bg);
    if (!botAI || !bot || !eyeBg)
        return false;

    TeamId const team = RTG_GetEffectiveBgTeam(bot);
    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return false;

    uint32 const stableRole = RTG_StableRole(botAI, bot);
    uint32 const seed = bot->GetGUID().GetCounter();
    uint32 const teamSize = std::max<uint32>(1, RTG_TeamSize(bg, team));
    uint32 const ownedCount = RTG_EotsOwnedCount(eyeBg, team);
    uint32 const homeOwnedCount = RTG_EotsHomeOwnedCount(eyeBg, team);
    PositionInfo centerPos;
    bool const centerSpawned = RTG_EotsCenterFlagSpawned(bg, centerPos);

    out.valid = true;
    out.assignedAtMs = getMSTime();
    out.minCommitMs = sPlayerbotAIConfig.rtgBgEotsCommitMs;

    if (RTG_EotsNeedsStartExit(bot, team))
    {
        out.role = RTGBgObjectiveRole::ExitSpawn;
        out.objectiveId = 0;
        RTG_SetDestination(out, RTG_EotsStartExitDestination(team), bg->GetMapId());
        out.minCommitMs = 12000;
        out.emergency = true;
        out.reason = "ExitStartRock";
        return true;
    }

    if (RTG_IsEotsFlagCarrier(bot, eyeBg))
    {
        uint32 nodeId = 0;
        PositionInfo pos;
        if (RTG_EotsNearestOwnedTower(bot, eyeBg, team, nodeId, pos))
        {
            out.role = RTGBgObjectiveRole::CarrierTurnIn;
            out.objectiveId = nodeId;
            RTG_SetDestination(out, pos, bg->GetMapId());
            out.minCommitMs = 5000;
            out.emergency = true;
            out.reason = "CarrierTurnInOwnedTower";
            return true;
        }

        uint32 const homeNode = RTG_EotsAssignedHomeNode(eyeBg, team, stableRole, seed);
        if (RTGTowerDef const* tower = RTG_EotsTower(homeNode))
        {
            out.role = RTGBgObjectiveRole::CaptureTower;
            out.objectiveId = homeNode;
            RTG_SetDestination(out, RTG_EotsTowerDestination(bot, team, *tower), bg->GetMapId());
            out.minCommitMs = 30000;
            out.emergency = true;
            out.reason = "CarrierNeedsOwnedTower";
            return true;
        }

        out.role = RTGBgObjectiveRole::MidfieldPressure;
        out.objectiveId = 0;
        RTG_SetDestination(out, centerPos, bg->GetMapId());
        out.minCommitMs = 8000;
        out.emergency = true;
        out.reason = "CarrierNoOwnedTowerHoldCenter";
        return true;
    }

    Unit* enemyFC = RTG_GetUnitValue(botAI, "enemy flag carrier");
    float const enemyFCDist = RTG_UnitValidForObjective(bot, enemyFC) ? bot->GetDistance(enemyFC) : FLT_MAX;
    bool const closeEnemyFC = enemyFCDist < 115.0f;
    bool const assignedEnemyFCInterceptor = enemyFCDist < 170.0f && (stableRole % 3u) != 0u;
    if (closeEnemyFC || assignedEnemyFCInterceptor)
    {
        out.role = RTGBgObjectiveRole::KillEnemyFlagCarrier;
        out.objectiveId = uint32(enemyFC->GetGUID().GetCounter());
        out.targetGuid = enemyFC->GetGUID();
        out.destination.Set(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ(), bg->GetMapId());
        out.minCommitMs = closeEnemyFC ? 8000 : 10000;
        out.emergency = true;
        out.reason = closeEnemyFC ? "EnemyFlagCarrierNearby" : "EnemyFlagCarrierEscaping";
        return true;
    }

    if (homeOwnedCount < 2)
    {
        if (homeOwnedCount >= 1 &&
            RTG_EotsShouldRunCenterFlag(bot, stableRole, seed, ownedCount, homeOwnedCount, centerSpawned, teamSize))
        {
            out.role = RTGBgObjectiveRole::CaptureEnemyFlag;
            out.objectiveId = 0;
            RTG_SetDestination(out, centerPos, bg->GetMapId());
            out.reason = "OpeningCenterFlagSplit";
            return true;
        }

        if (homeOwnedCount >= 1 && RTG_EotsShouldPressureEnemyWithOneHome(stableRole, seed, teamSize))
        {
            uint32 const attackNode = RTG_EotsAssignedEnemyNode(eyeBg, team, stableRole, seed);
            if (RTGTowerDef const* tower = RTG_EotsTower(attackNode))
            {
                out.role = RTGBgObjectiveRole::CaptureTower;
                out.objectiveId = attackNode;
                RTG_SetDestination(out, RTG_EotsTowerDestination(bot, team, *tower), bg->GetMapId());
                out.minCommitMs = 22000;
                out.reason = std::string("OneHomeAwayPressure") + tower->name;
                return true;
            }
        }

        uint32 const nodeId = RTG_EotsAssignedHomeNode(eyeBg, team, stableRole, seed);
        if (RTGTowerDef const* tower = RTG_EotsTower(nodeId))
        {
            out.role = RTGBgObjectiveRole::CaptureTower;
            out.objectiveId = nodeId;
            RTG_SetDestination(out, RTG_EotsTowerDestination(bot, team, *tower), bg->GetMapId());
            out.minCommitMs = 30000;
            out.reason = nodeId == POINT_FEL_REAVER ? "NeedHomeTowerFRR" : "NeedSecondHomeTower";
            return true;
        }
    }

    Unit* friendlyFC = RTG_GetUnitValue(botAI, "team flag carrier");
    Unit* enemyPlayer = RTG_GetUnitValue(botAI, "enemy player target");
    bool friendlyCarrierThreatened = RTG_UnitValidForObjective(bot, friendlyFC) &&
        RTG_UnitValidForObjective(bot, enemyPlayer) && enemyPlayer->GetDistance(friendlyFC) < 45.0f;
    if (friendlyCarrierThreatened && (stableRole % 8u) == 0u)
    {
        out.role = RTGBgObjectiveRole::EscortFriendlyFlagCarrier;
        out.objectiveId = uint32(friendlyFC->GetGUID().GetCounter());
        out.targetGuid = friendlyFC->GetGUID();
        out.destination.Set(friendlyFC->GetPositionX(), friendlyFC->GetPositionY(), friendlyFC->GetPositionZ(), bg->GetMapId());
        out.minCommitMs = 8000;
        out.emergency = true;
        out.reason = "FriendlyCarrierThreatened";
        return true;
    }

    if (ownedCount >= 2 &&
        RTG_EotsShouldRunCenterFlag(bot, stableRole, seed, ownedCount, homeOwnedCount, centerSpawned, teamSize))
    {
        out.role = RTGBgObjectiveRole::CaptureEnemyFlag;
        out.objectiveId = 0;
        RTG_SetDestination(out, centerPos, bg->GetMapId());
        out.reason = ownedCount >= 3 ? "CenterFlagWithTowerLead" : "CenterFlagAtTwoTowers";
        return true;
    }

    uint32 attackNode = RTG_EotsAssignedEnemyNode(eyeBg, team, stableRole, seed);
    if (attackNode)
    {
        if (RTGTowerDef const* tower = RTG_EotsTower(attackNode))
        {
            out.role = RTGBgObjectiveRole::CaptureTower;
            out.objectiveId = attackNode;
            RTG_SetDestination(out, RTG_EotsTowerDestination(bot, team, *tower), bg->GetMapId());
            out.minCommitMs = 30000;
            out.reason = std::string("Contest") + tower->name;
            return true;
        }
    }

    if (RTG_EotsShouldDefendTower(stableRole, ownedCount, teamSize))
    {
        uint32 const nodeId = RTG_EotsAssignedDefenseNode(eyeBg, team, stableRole, seed);
        if (RTGTowerDef const* tower = RTG_EotsTower(nodeId))
        {
            out.role = RTGBgObjectiveRole::DefendTower;
            out.objectiveId = nodeId;
            RTG_SetDestination(out, tower->defense, bg->GetMapId());
            out.minCommitMs = 12000;
            out.reason = "AssignedTowerDefenseFallback";
            return true;
        }
    }

    uint32 defendNode = RTG_EotsAssignedDefenseNode(eyeBg, team, stableRole, seed);
    if (defendNode)
    {
        if (RTGTowerDef const* tower = RTG_EotsTower(defendNode))
        {
            out.role = RTGBgObjectiveRole::DefendTower;
            out.objectiveId = defendNode;
            RTG_SetDestination(out, tower->defense, bg->GetMapId());
            out.reason = "FallbackTowerDefense";
            return true;
        }
    }

    out.role = RTGBgObjectiveRole::MidfieldPressure;
    out.objectiveId = 0;
    RTG_SetDestination(out, centerPos, bg->GetMapId());
    out.reason = "FallbackMidfield";
    return true;
}

bool RTG_AssignmentStillValid(PlayerbotAI* botAI, Player* bot, Battleground* bg, RTGBgObjectiveAssignment const& assignment)
{
    if (!botAI || !bot || !bg || !assignment.valid || assignment.role == RTGBgObjectiveRole::None)
        return false;

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);
    TeamId const team = RTG_GetEffectiveBgTeam(bot);
    uint32 const elapsed = getMSTimeDiff(assignment.assignedAtMs, getMSTime());

    if (assignment.role == RTGBgObjectiveRole::StuckRecover)
        return elapsed < assignment.minCommitMs;

    if (bgType == BATTLEGROUND_WS)
    {
        BattlegroundWS* wsBg = dynamic_cast<BattlegroundWS*>(bg);
        if (!wsBg)
            return false;

        TeamId const enemyTeam = wsBg->GetOtherTeamId(team);
        switch (assignment.role)
        {
            case RTGBgObjectiveRole::ReturnOwnFlag:
                return wsBg->GetFlagState(team) != BG_WS_FLAG_STATE_ON_BASE &&
                       wsBg->GetFlagState(team) != BG_WS_FLAG_STATE_ON_PLAYER;
            case RTGBgObjectiveRole::CaptureEnemyFlag:
                return !RTG_IsWsgFlagCarrier(bot, wsBg, team) &&
                       wsBg->GetFlagState(enemyTeam) == BG_WS_FLAG_STATE_ON_BASE;
            case RTGBgObjectiveRole::KillEnemyFlagCarrier:
            case RTGBgObjectiveRole::EscortFriendlyFlagCarrier:
                return RTG_TargetAssignmentStillValid(botAI, bot, assignment.targetGuid) &&
                       elapsed < std::max<uint32>(assignment.minCommitMs, 8000);
            case RTGBgObjectiveRole::MidfieldPressure:
                if (!assignment.targetGuid.IsEmpty())
                    return RTG_TargetAssignmentStillValid(botAI, bot, assignment.targetGuid) &&
                           elapsed < std::max<uint32>(assignment.minCommitMs, 6000);
                return elapsed < std::max<uint32>(assignment.minCommitMs, 8000);
            default:
                return true;
        }
    }

    if (bgType == BATTLEGROUND_EY)
    {
        BattlegroundEY* eyeBg = dynamic_cast<BattlegroundEY*>(bg);
        if (!eyeBg)
            return false;

        switch (assignment.role)
        {
            case RTGBgObjectiveRole::ExitSpawn:
                return RTG_EotsNeedsStartExit(bot, team) &&
                       elapsed < std::max<uint32>(assignment.minCommitMs, 12000);
            case RTGBgObjectiveRole::CarrierTurnIn:
                return RTG_IsEotsFlagCarrier(bot, eyeBg) && RTG_EotsOwnedCount(eyeBg, team) > 0;
            case RTGBgObjectiveRole::CaptureEnemyFlag:
            {
                PositionInfo centerPos;
                return !RTG_IsEotsFlagCarrier(bot, eyeBg) && RTG_EotsCenterFlagSpawned(bg, centerPos);
            }
            case RTGBgObjectiveRole::CaptureTower:
                if (!assignment.objectiveId)
                    return true;
                if (RTG_EotsNodeOwned(eyeBg, assignment.objectiveId, team))
                    return elapsed < 8000;
                return true;
            case RTGBgObjectiveRole::DefendTower:
                return RTG_EotsNodeOwned(eyeBg, assignment.objectiveId, team) &&
                       elapsed < 12000;
            case RTGBgObjectiveRole::KillEnemyFlagCarrier:
            case RTGBgObjectiveRole::EscortFriendlyFlagCarrier:
                return RTG_TargetAssignmentStillValid(botAI, bot, assignment.targetGuid) &&
                       elapsed < std::max<uint32>(assignment.minCommitMs, 8000);
            case RTGBgObjectiveRole::MidfieldPressure:
                return elapsed < 8000;
            default:
                return true;
        }
    }

    return false;
}

bool RTG_SameObjective(RTGBgObjectiveAssignment const& a, RTGBgObjectiveAssignment const& b)
{
    return a.role == b.role && a.objectiveId == b.objectiveId && a.targetGuid == b.targetGuid;
}

PositionInfo RTG_RecoveryPoint(Player* bot, Battleground* bg, RTGBgObjectiveAssignment const& current)
{
    TeamId const team = RTG_GetEffectiveBgTeam(bot);
    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);

    if (bgType == BATTLEGROUND_WS)
    {
        if (current.role == RTGBgObjectiveRole::MidfieldPressure)
        {
            if (current.destination.valueSet)
                return current.destination;

            if (current.objectiveId == RTG_OBJECTIVE_WSG_HUNT_OWN_FLAG)
                return RTG_WsgEnemyCarrierSearchDestination(team);

            return RTG_WsgEnemyFlagRoom(team);
        }

        if (current.role == RTGBgObjectiveRole::CaptureEnemyFlag)
            return WSG_MIDFIELD;

        return RTG_WsgBaseExit(team);
    }

    if (bgType == BATTLEGROUND_EY)
    {
        if (RTGTowerDef const* tower = RTG_EotsTower(current.objectiveId))
            return tower->nodeId == POINT_FEL_REAVER ? (team == TEAM_HORDE ? EOTS_HORDE_ROAD_FORK : tower->allianceApproach)
                                                     : (team == TEAM_HORDE ? tower->hordeApproach : tower->allianceApproach);

        return team == TEAM_HORDE ? EOTS_HORDE_ROAD_FORK : EOTS_ALLIANCE_ROAD_FORK;
    }

    PositionInfo fallback;
    if (bot)
        fallback.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    return fallback;
}

bool RTG_IsTowerRole(RTGBgObjectiveRole role)
{
    return role == RTGBgObjectiveRole::CaptureTower || role == RTGBgObjectiveRole::DefendTower;
}

bool RTG_BuildObjective(PlayerbotAI* botAI, Player* bot, Battleground* bg, RTGBgObjectiveAssignment& assignment)
{
    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);
    if (bgType == BATTLEGROUND_WS)
        return RTG_SelectWsgObjective(botAI, bot, bg, assignment);

    if (bgType == BATTLEGROUND_EY)
        return RTG_SelectEotsObjective(botAI, bot, bg, assignment);

    return false;
}

} // namespace

TeamId RTG_GetEffectiveBgTeam(Player* player)
{
    if (!player)
        return TEAM_NEUTRAL;

    if (player->InBattleground())
    {
        Battleground* bg = player->GetBattleground();
        BattlegroundTypeId const bgType = RTG_GetRealBgType(bg);
        TeamId const bgTeam = player->GetBgTeamId();
        if (bgTeam == TEAM_ALLIANCE || bgTeam == TEAM_HORDE)
        {
            if (bgType != BATTLEGROUND_EY)
                return bgTeam;
        }

        if (bgType == BATTLEGROUND_EY && bg)
        {
            static std::unordered_map<uint64, TeamId> lockedEotsSideByBot;
            uint64 const key = RTG_StateKey(player, bg);

            auto itr = lockedEotsSideByBot.find(key);
            if (itr != lockedEotsSideByBot.end() &&
                (itr->second == TEAM_ALLIANCE || itr->second == TEAM_HORDE))
                return itr->second;

            float const hordeStartDist = RTG_Distance3d(player, EOTS_HORDE_START);
            float const allianceStartDist = RTG_Distance3d(player, EOTS_ALLIANCE_START);
            bool const onStartRockOrExit = player->GetPositionZ() > 1230.0f ||
                hordeStartDist < 360.0f || allianceStartDist < 360.0f;

            if (onStartRockOrExit)
            {
                TeamId const inferred = hordeStartDist <= allianceStartDist ? TEAM_HORDE : TEAM_ALLIANCE;
                lockedEotsSideByBot[key] = inferred;
                return inferred;
            }

            if (bgTeam == TEAM_ALLIANCE || bgTeam == TEAM_HORDE)
                return bgTeam;
        }
        else if (bgTeam == TEAM_ALLIANCE || bgTeam == TEAM_HORDE)
        {
            return bgTeam;
        }
    }

    TeamId const team = player->GetTeamId();
    if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
        return team;

    return TEAM_NEUTRAL;
}

bool RTG_IsSameEffectiveBgTeam(Player* a, Player* b)
{
    TeamId const aTeam = RTG_GetEffectiveBgTeam(a);
    TeamId const bTeam = RTG_GetEffectiveBgTeam(b);
    return aTeam != TEAM_NEUTRAL && aTeam == bTeam;
}

bool RTG_IsEnemyEffectiveBgTeam(Player* a, Player* b)
{
    TeamId const aTeam = RTG_GetEffectiveBgTeam(a);
    TeamId const bTeam = RTG_GetEffectiveBgTeam(b);
    return aTeam != TEAM_NEUTRAL && bTeam != TEAM_NEUTRAL && aTeam != bTeam;
}

char const* RTG_BgObjectiveRoleName(RTGBgObjectiveRole role)
{
    switch (role)
    {
        case RTGBgObjectiveRole::AttackObjective:
            return "AttackObjective";
        case RTGBgObjectiveRole::DefendObjective:
            return "DefendObjective";
        case RTGBgObjectiveRole::EscortFriendlyFlagCarrier:
            return "EscortFriendlyFlagCarrier";
        case RTGBgObjectiveRole::KillEnemyFlagCarrier:
            return "KillEnemyFlagCarrier";
        case RTGBgObjectiveRole::ReturnOwnFlag:
            return "ReturnOwnFlag";
        case RTGBgObjectiveRole::CaptureEnemyFlag:
            return "CaptureEnemyFlag";
        case RTGBgObjectiveRole::CaptureTower:
            return "CaptureTower";
        case RTGBgObjectiveRole::DefendTower:
            return "DefendTower";
        case RTGBgObjectiveRole::MidfieldPressure:
            return "MidfieldPressure";
        case RTGBgObjectiveRole::CarrierTurnIn:
            return "CarrierTurnIn";
        case RTGBgObjectiveRole::ExitSpawn:
            return "ExitSpawn";
        case RTGBgObjectiveRole::StuckRecover:
            return "StuckRecover";
        default:
            return "None";
    }
}

bool RTG_BgObjectiveBrainEnabledFor(Player* bot)
{
    if (!sPlayerbotAIConfig.rtgBgObjectiveBrain || !bot)
        return false;

    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);
    return bgType == BATTLEGROUND_WS || bgType == BATTLEGROUND_EY;
}

bool RTG_SelectBattlegroundObjective(PlayerbotAI* botAI, RTGBgObjectiveAssignment& assignment)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!RTG_BgObjectiveBrainEnabledFor(bot) || !bot->IsAlive())
    {
        RTG_ClearBattlegroundObjective(botAI);
        return false;
    }

    Battleground* bg = bot->GetBattleground();
    uint64 const key = RTG_StateKey(bot, bg);
    if (!key)
        return false;

    RTGBgObjectiveAssignment candidate;
    if (!RTG_BuildObjective(botAI, bot, bg, candidate) || !candidate.valid || !candidate.destination.valueSet)
        return false;

    RTGObjectiveBrainState& state = RTGObjectiveStates[key];
    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);
    TeamId const team = RTG_GetEffectiveBgTeam(bot);
    bool const stateMatches = state.bgInstance == bg->GetInstanceID() && state.bgType == uint32(bgType) && state.team == team;
    bool const currentValid = stateMatches && RTG_AssignmentStillValid(botAI, bot, bg, state.assignment);
    bool const sameObjective = currentValid && RTG_SameObjective(state.assignment, candidate);
    uint32 const now = getMSTime();

    if (sameObjective)
    {
        state.assignment.destination = candidate.destination;
        state.assignment.reason = candidate.reason;
        state.assignment.emergency = candidate.emergency;
        assignment = state.assignment;
        return true;
    }

    if (currentValid && !candidate.emergency &&
        getMSTimeDiff(state.assignment.assignedAtMs, now) < state.assignment.minCommitMs)
    {
        assignment = state.assignment;
        RTG_Debug(bot, bg, assignment, "role-change blocked");
        return true;
    }

    uint32 const jitter = bot->GetGUID().GetCounter() % 3000u;
    candidate.assignedAtMs = now;
    candidate.minCommitMs = std::max<uint32>(candidate.minCommitMs, bgType == BATTLEGROUND_EY
        ? sPlayerbotAIConfig.rtgBgEotsCommitMs
        : sPlayerbotAIConfig.rtgBgWsgCommitMs);
    candidate.minCommitMs += jitter;

    state.assignment = candidate;
    state.bgInstance = bg->GetInstanceID();
    state.bgType = uint32(bgType);
    state.team = team;
    state.progressInitialized = false;

    assignment = state.assignment;
    RTG_Debug(bot, bg, assignment, candidate.emergency ? "emergency" : "select");
    return true;
}

bool RTG_UpdateBattlegroundObjectiveStuck(PlayerbotAI* botAI, PositionInfo const& currentObjective,
                                          RTGBgObjectiveAssignment& assignment)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!RTG_BgObjectiveBrainEnabledFor(bot) || !bot->IsAlive() || !currentObjective.valueSet)
        return false;

    if (bot->IsInCombat())
        return false;

    Battleground* bg = bot->GetBattleground();
    uint64 const key = RTG_StateKey(bot, bg);
    if (!key)
        return false;

    RTGObjectiveBrainState& state = RTGObjectiveStates[key];
    if (!RTG_AssignmentStillValid(botAI, bot, bg, state.assignment))
        return false;

    uint32 const now = getMSTime();
    bool const objectiveChanged = !state.trackedObjective.valueSet ||
        state.trackedObjective.mapId != currentObjective.mapId ||
        std::fabs(state.trackedObjective.x - currentObjective.x) > 6.0f ||
        std::fabs(state.trackedObjective.y - currentObjective.y) > 6.0f ||
        std::fabs(state.trackedObjective.z - currentObjective.z) > 8.0f;

    auto resetProgress = [&]()
    {
        state.trackedObjective = currentObjective;
        state.lastX = bot->GetPositionX();
        state.lastY = bot->GetPositionY();
        state.lastZ = bot->GetPositionZ();
        state.lastProgressMs = now;
        state.stuckSinceMs = 0;
        state.progressInitialized = true;
    };

    if (!state.progressInitialized || objectiveChanged)
    {
        resetProgress();
        return false;
    }

    if (getMSTimeDiff(state.lastProgressMs, now) < 2500)
        return false;

    float const moved = bot->GetExactDist(state.lastX, state.lastY, state.lastZ);
    float const distToObjective = bot->GetExactDist(currentObjective.x, currentObjective.y, currentObjective.z);
    if (moved > 2.25f || distToObjective < 7.0f)
    {
        resetProgress();
        return false;
    }

    if (!state.stuckSinceMs)
    {
        state.stuckSinceMs = now;
        return false;
    }

    if (getMSTimeDiff(state.stuckSinceMs, now) < sPlayerbotAIConfig.rtgBgStuckRecoverMs)
        return false;

    RTGBgObjectiveAssignment recovery = state.assignment;
    recovery.role = RTGBgObjectiveRole::StuckRecover;
    recovery.destination = RTG_RecoveryPoint(bot, bg, state.assignment);
    recovery.assignedAtMs = now;
    recovery.minCommitMs = 5000;
    recovery.emergency = true;
    recovery.valid = true;
    recovery.reason = "StuckRecover";

    state.assignment = recovery;
    resetProgress();
    assignment = recovery;
    RTG_Debug(bot, bg, assignment, "stuck-recover");
    return true;
}

bool RTG_ShouldHoldBattlegroundObjective(PlayerbotAI* botAI, PositionInfo const& currentObjective)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!RTG_BgObjectiveBrainEnabledFor(bot) || !currentObjective.valueSet)
        return false;

    Battleground* bg = bot->GetBattleground();
    uint64 const key = RTG_StateKey(bot, bg);
    if (!key)
        return false;

    auto itr = RTGObjectiveStates.find(key);
    if (itr == RTGObjectiveStates.end())
        return false;

    RTGBgObjectiveAssignment const& assignment = itr->second.assignment;
    BattlegroundTypeId bgType = RTG_GetRealBgType(bg);

    if (!RTG_AssignmentStillValid(botAI, bot, bg, assignment))
        return false;

    if (bgType == BATTLEGROUND_EY && RTG_IsTowerRole(assignment.role))
    {
        if (bot->GetExactDist2d(currentObjective.x, currentObjective.y) <= 8.0f &&
            std::fabs(bot->GetPositionZ() - currentObjective.z) <= 10.0f)
        {
            RTG_Debug(bot, bg, assignment, "hold");
            return true;
        }
    }

    if (bgType == BATTLEGROUND_WS && assignment.role == RTGBgObjectiveRole::DefendObjective)
    {
        if (bot->GetExactDist2d(currentObjective.x, currentObjective.y) <= 8.0f &&
            std::fabs(bot->GetPositionZ() - currentObjective.z) <= 12.0f)
            return true;
    }

    return false;
}

void RTG_ClearBattlegroundObjective(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    Battleground* bg = bot->GetBattleground();
    uint64 const key = RTG_StateKey(bot, bg);
    if (key)
    {
        RTGObjectiveStates.erase(key);
        RTGTeamSizeCache.erase((uint64(bg->GetInstanceID()) << 8) | uint64(uint32(TEAM_ALLIANCE)));
        RTGTeamSizeCache.erase((uint64(bg->GetInstanceID()) << 8) | uint64(uint32(TEAM_HORDE)));
        return;
    }

    // Once GetBattleground() is null the old key cannot be reconstructed. Remove
    // any state whose low 32 bits belong to this bot so completed BG instances do
    // not accumulate stale objective records for the lifetime of worldserver.
    uint32 const guidLow = bot->GetGUID().GetCounter();
    for (auto itr = RTGObjectiveStates.begin(); itr != RTGObjectiveStates.end();)
    {
        if (uint32(itr->first & 0xFFFFFFFFu) == guidLow)
            itr = RTGObjectiveStates.erase(itr);
        else
            ++itr;
    }
}
