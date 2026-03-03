#include "HealerHoldDpsAction.h"

#include "Group.h"
#include "Playerbots.h"

bool HealerHoldDpsAction::PartyNeedsHealing(float hpThreshold) const
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (!m || !m->IsInWorld())
            continue;

        if (m->GetHealthPct() < hpThreshold)
            return true;
    }

    return false;
}

bool HealerHoldDpsAction::Execute(Event /*event*/)
{
    // Only applies to healer bots
    if (!botAI->IsHeal(bot))
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // If we aren't fighting anything, nothing to stop
    Unit* victim = bot->GetVictim();
    if (!victim)
        return false;

    // Self defense: if the mob is actually hitting the healer, allow fighting back
	if (victim->GetVictim() == bot)
		return false;

	// Strict mode: healers do not DPS in dungeons
	bot->AttackStop();
	bot->SetTarget(ObjectGuid::Empty);
	return true;

    // Also stop DPS if mana is not high (forces conserve + heal-only posture)
    if (bot->GetMaxPower(POWER_MANA) > 0)
    {
        float manaPct = 100.0f * float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA));
        if (manaPct < 85.0f)
        {
            bot->AttackStop();
            bot->SetTarget(ObjectGuid::Empty);
            return true;
        }
    }

    // Otherwise: party is stable and healer has lots of mana — still stop DPS if you want “heals only”
    // Uncomment the next lines to make it STRICTLY heals-only no matter what:
    /*
    bot->AttackStop();
    bot->SetTarget(ObjectGuid::Empty);
    return true;
    */

    return false;
}