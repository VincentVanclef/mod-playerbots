#pragma once

#include "Define.h"
#include "ObjectGuid.h"

class Player;

namespace RTG
{
bool ClassCanRole(uint8 cls, uint32 role);
uint32 DefaultRoleForClass(uint8 cls);
uint8 DefaultSpecTabForClass(uint8 cls);
uint32 RoleForClassSpecTab(uint8 cls, uint8 specTab);
bool PreferredSpecTabForClassRole(uint8 cls, uint32 role, uint8& specTab);
bool GetOfflineSpecTab(ObjectGuid::LowType guid, uint8 cls, uint8& specTab);
bool HasOfflineSpecData(ObjectGuid::LowType guid, uint8 cls, uint8* specTab = nullptr);
uint32 GetOfflineSpecRole(ObjectGuid::LowType guid, uint8 cls);
bool OfflineSpecCanPerformRole(ObjectGuid::LowType guid, uint8 cls, uint32 role);
uint32 GetActualSpecRole(Player* bot);
bool ActualSpecCanPerformRole(Player* bot, uint32 role);
uint32 NormalizeQueuedRoleMask(uint32 roleMask);
uint32 TargetLfgRoleCount(uint32 role);
uint32 ActualRoleForBot(Player* bot);
}
