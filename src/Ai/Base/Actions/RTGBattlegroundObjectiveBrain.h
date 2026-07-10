/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef PLAYERBOTS_RTG_BATTLEGROUND_OBJECTIVE_BRAIN_H
#define PLAYERBOTS_RTG_BATTLEGROUND_OBJECTIVE_BRAIN_H

#include <string>

#include "ObjectGuid.h"
#include "PositionValue.h"
#include "SharedDefines.h"

class Player;
class PlayerbotAI;

enum class RTGBgObjectiveRole : uint8
{
    None,
    AttackObjective,
    DefendObjective,
    EscortFriendlyFlagCarrier,
    KillEnemyFlagCarrier,
    ReturnOwnFlag,
    CaptureEnemyFlag,
    CaptureTower,
    DefendTower,
    MidfieldPressure,
    CarrierTurnIn,
    StuckRecover
};

struct RTGBgObjectiveAssignment
{
    RTGBgObjectiveRole role = RTGBgObjectiveRole::None;
    PositionInfo destination;
    ObjectGuid targetGuid;
    uint32 assignedAtMs = 0;
    uint32 minCommitMs = 15000;
    uint32 objectiveId = 0;
    bool emergency = false;
    bool valid = false;
    std::string reason;
};

TeamId RTG_GetEffectiveBgTeam(Player* player);
bool RTG_IsSameEffectiveBgTeam(Player* a, Player* b);
bool RTG_IsEnemyEffectiveBgTeam(Player* a, Player* b);

char const* RTG_BgObjectiveRoleName(RTGBgObjectiveRole role);
bool RTG_BgObjectiveBrainEnabledFor(Player* bot);
bool RTG_SelectBattlegroundObjective(PlayerbotAI* botAI, RTGBgObjectiveAssignment& assignment);
bool RTG_UpdateBattlegroundObjectiveStuck(PlayerbotAI* botAI, PositionInfo const& currentObjective,
                                          RTGBgObjectiveAssignment& assignment);
bool RTG_ShouldHoldBattlegroundObjective(PlayerbotAI* botAI, PositionInfo const& currentObjective);
void RTG_ClearBattlegroundObjective(PlayerbotAI* botAI);

#endif
