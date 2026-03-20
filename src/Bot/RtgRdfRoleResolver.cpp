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

uint8 DefaultSpecTabForClass(uint8 cls)
{
    switch (cls)
    {
        case CLASS_MAGE: return MAGE_TAB_FROST;
        case CLASS_PALADIN: return PALADIN_TAB_RETRIBUTION;
        case CLASS_PRIEST: return PRIEST_TAB_HOLY;
        case CLASS_WARLOCK: return WARLOCK_TAB_DEMONOLOGY;
        case CLASS_SHAMAN: return SHAMAN_TAB_ELEMENTAL;
        default: return 0;
    }
}

uint32 RoleForClassSpecTab(uint8 cls, uint8 specTab)
{
    switch (cls)
    {
        case CLASS_DRUID:
            if (specTab == DRUID_TAB_RESTORATION)
                return lfg::PLAYER_ROLE_HEALER;
            if (specTab == DRUID_TAB_FERAL)
                return lfg::PLAYER_ROLE_TANK;
            return lfg::PLAYER_ROLE_DAMAGE;
        case CLASS_PALADIN:
            if (specTab == PALADIN_TAB_HOLY)
                return lfg::PLAYER_ROLE_HEALER;
            if (specTab == PALADIN_TAB_PROTECTION)
                return lfg::PLAYER_ROLE_TANK;
            return lfg::PLAYER_ROLE_DAMAGE;
        case CLASS_PRIEST:
            return specTab == PRIEST_TAB_SHADOW ? lfg::PLAYER_ROLE_DAMAGE : lfg::PLAYER_ROLE_HEALER;
        case CLASS_SHAMAN:
            return specTab == SHAMAN_TAB_RESTORATION ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
        case CLASS_WARRIOR:
            return specTab == WARRIOR_TAB_PROTECTION ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
        case CLASS_DEATH_KNIGHT:
            return specTab == DEATH_KNIGHT_TAB_BLOOD ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
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

    std::map<uint8, uint32> tabs = {{0, 0}, {1, 0}, {2, 0}};
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

    return RoleForClassSpecTab(bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
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
        case lfg::PLAYER_ROLE_TANK: return 1u;
        case lfg::PLAYER_ROLE_HEALER: return 1u;
        case lfg::PLAYER_ROLE_DAMAGE: return 3u;
        default: return 0u;
    }
}

uint32 ActualRoleForBot(Player* bot)
{
    return GetActualSpecRole(bot);
}
}
