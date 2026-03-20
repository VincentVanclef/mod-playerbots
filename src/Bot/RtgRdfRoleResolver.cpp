#include "RtgRdfRoleResolver.h"

#include "AiFactory.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DatabaseEnv.h"
#include "LFGMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "SharedDefines.h"

#include <map>

namespace RTG
{
bool ClassCanRole(uint8 cls, uint32 role)
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

uint32 DefaultRoleForClass(uint8 cls)
{
    if (ClassCanRole(cls, lfg::PLAYER_ROLE_HEALER))
        return lfg::PLAYER_ROLE_HEALER;
    if (ClassCanRole(cls, lfg::PLAYER_ROLE_TANK))
        return lfg::PLAYER_ROLE_TANK;
    return lfg::PLAYER_ROLE_DAMAGE;
}

// Spec tab indices only:
// 0 = first tab, 1 = second tab, 2 = third tab
uint8 DefaultSpecTabForClass(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:      return 0; // Arms
        case CLASS_PALADIN:      return 2; // Retribution
        case CLASS_HUNTER:       return 0; // Beast Mastery
        case CLASS_ROGUE:        return 1; // Combat
        case CLASS_PRIEST:       return 0; // Discipline
        case CLASS_DEATH_KNIGHT: return 2; // Unholy
        case CLASS_SHAMAN:       return 0; // Elemental
        case CLASS_MAGE:         return 2; // Frost
        case CLASS_WARLOCK:      return 2; // Destruction
        case CLASS_DRUID:        return 0; // Balance
        default:                 return 0;
    }
}

uint32 RoleForClassSpecTab(uint8 cls, uint8 specTab)
{
    switch (cls)
    {
        case CLASS_DRUID:
            // 0 Balance, 1 Feral, 2 Restoration
            if (specTab == 2)
                return lfg::PLAYER_ROLE_HEALER;
            if (specTab == 1)
                return lfg::PLAYER_ROLE_TANK;
            return lfg::PLAYER_ROLE_DAMAGE;

        case CLASS_PALADIN:
            // 0 Holy, 1 Protection, 2 Retribution
            if (specTab == 0)
                return lfg::PLAYER_ROLE_HEALER;
            if (specTab == 1)
                return lfg::PLAYER_ROLE_TANK;
            return lfg::PLAYER_ROLE_DAMAGE;

        case CLASS_PRIEST:
            // 0 Discipline, 1 Holy, 2 Shadow
            return (specTab == 2) ? lfg::PLAYER_ROLE_DAMAGE : lfg::PLAYER_ROLE_HEALER;

        case CLASS_SHAMAN:
            // 0 Elemental, 1 Enhancement, 2 Restoration
            return (specTab == 2) ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;

        case CLASS_WARRIOR:
            // 0 Arms, 1 Fury, 2 Protection
            return (specTab == 2) ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;

        case CLASS_DEATH_KNIGHT:
            // 0 Blood, 1 Frost, 2 Unholy
            return (specTab == 0) ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;

        default:
            return lfg::PLAYER_ROLE_DAMAGE;
    }
}

bool GetOfflineSpecTab(ObjectGuid::LowType guid, uint8 cls, uint8& specTab)
{
    specTab = DefaultSpecTabForClass(cls);

    QueryResult specResult = CharacterDatabase.Query("SELECT activeTalentGroup FROM characters WHERE guid = {}", guid);
    uint8 activeSpec = 0;
    if (specResult)
        activeSpec = specResult->Fetch()[0].Get<uint8>();

    uint32 activeMask = (1u << activeSpec);
    uint32 const* talentTabIds = GetTalentTabPages(cls);
    if (!talentTabIds)
        return true;

    std::map<uint8, uint32> tabs = { {0, 0}, {1, 0}, {2, 0} };

    QueryResult talentResult = CharacterDatabase.Query("SELECT spell, specMask FROM character_talent WHERE guid = {}", guid);
    if (!talentResult)
        return true;

    do
    {
        Field* fields = talentResult->Fetch();
        uint32 spellId = fields[0].Get<uint32>();
        uint8 specMask = fields[1].Get<uint8>();

        if ((activeMask & specMask) == 0)
            continue;

        TalentSpellPos const* talentPos = GetTalentSpellPos(spellId);
        if (!talentPos)
            continue;

        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentPos->talent_id);
        if (!talentInfo)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        uint32 rank = spellInfo ? spellInfo->GetRank() : 1u;

        if (talentInfo->TalentTab == talentTabIds[0])
            tabs[0] += rank;
        else if (talentInfo->TalentTab == talentTabIds[1])
            tabs[1] += rank;
        else if (talentInfo->TalentTab == talentTabIds[2])
            tabs[2] += rank;
    } while (talentResult->NextRow());

    if ((tabs[0] + tabs[1] + tabs[2]) == 0)
        return true;

    specTab = 0;
    uint32 maxPoints = tabs[0];

    for (uint8 i = 1; i < 3; ++i)
    {
        if (tabs[i] > maxPoints)
        {
            maxPoints = tabs[i];
            specTab = i;
        }
    }

    return true;
}

uint32 GetOfflineSpecRole(ObjectGuid::LowType guid, uint8 cls)
{
    uint8 specTab = 0;
    GetOfflineSpecTab(guid, cls, specTab);
    return RoleForClassSpecTab(cls, specTab);
}

uint32 GetActualSpecRole(Player* bot)
{
    if (!bot)
        return lfg::PLAYER_ROLE_DAMAGE;

    return RoleForClassSpecTab(bot->getClass(), static_cast<uint8>(AiFactory::GetPlayerSpecTab(bot)));
}

uint32 NormalizeQueuedRoleMask(uint32 roleMask)
{
    if (roleMask & lfg::PLAYER_ROLE_TANK)
        return lfg::PLAYER_ROLE_TANK;
    if (roleMask & lfg::PLAYER_ROLE_HEALER)
        return lfg::PLAYER_ROLE_HEALER;
    if (roleMask & lfg::PLAYER_ROLE_DAMAGE)
        return lfg::PLAYER_ROLE_DAMAGE;
    return 0u;
}

uint32 TargetLfgRoleCount(uint32 role)
{
    switch (role)
    {
        case lfg::PLAYER_ROLE_TANK:   return 1u;
        case lfg::PLAYER_ROLE_HEALER: return 1u;
        case lfg::PLAYER_ROLE_DAMAGE: return 3u;
        default:                      return 0u;
    }
}

uint32 ActualRoleForBot(Player* bot)
{
    return GetActualSpecRole(bot);
}
}