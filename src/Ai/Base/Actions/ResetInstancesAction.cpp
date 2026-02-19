/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ResetInstancesAction.h"

#include "InstancePackets.h" // WorldPackets::Instance::ResetInstances
#include "Playerbots.h"

bool ResetInstancesAction::Execute(Event event)
{
    // AzerothCore switched this handler to use a typed WorldPackets packet
    // instead of the old generic WorldPacket.
    WorldPackets::Instance::ResetInstances packet;
    bot->GetSession()->HandleResetInstancesOpcode(packet);

    botAI->TellMaster("Resetting all instances");
    return true;
}

bool ResetInstancesAction::isUseful() { return botAI->GetGroupLeader() == bot; };
