// PATCH: Phase 3 Acquire Role Enforcement + Reservation

static uint32 RTG_SelectDesiredRole(uint32 owner)
{
    uint32 needTank = sRandomPlayerbotMgr.RTG_GetBotEventValue(owner, "rtg_lfg_need_tank");
    uint32 needHeal = sRandomPlayerbotMgr.RTG_GetBotEventValue(owner, "rtg_lfg_need_heal");
    uint32 needDps  = sRandomPlayerbotMgr.RTG_GetBotEventValue(owner, "rtg_lfg_need_dps");

    if (needTank > 0)
        return lfg::PLAYER_ROLE_TANK;
    if (needHeal > 0)
        return lfg::PLAYER_ROLE_HEALER;
    return lfg::PLAYER_ROLE_DAMAGE;
}

static bool RTG_BotCanPerformRole(Player* bot, uint32 role)
{
    if (!bot)
        return false;

    uint8 cls = bot->getClass();

    switch (role)
    {
        case lfg::PLAYER_ROLE_TANK:
            return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_DEATH_KNIGHT;
        case lfg::PLAYER_ROLE_HEALER:
            return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
        case lfg::PLAYER_ROLE_DAMAGE:
            return true;
    }
    return false;
}

static void RTG_ConsumeRoleDemand(uint32 owner, uint32 role)
{
    const char* key = nullptr;

    switch (role)
    {
        case lfg::PLAYER_ROLE_TANK:   key = "rtg_lfg_need_tank"; break;
        case lfg::PLAYER_ROLE_HEALER: key = "rtg_lfg_need_heal"; break;
        case lfg::PLAYER_ROLE_DAMAGE: key = "rtg_lfg_need_dps"; break;
    }

    if (!key)
        return;

    uint32 val = sRandomPlayerbotMgr.RTG_GetBotEventValue(owner, key);
    if (val > 0)
        sRandomPlayerbotMgr.RTG_SetBotEventValue(owner, key, val - 1, 30);
}
