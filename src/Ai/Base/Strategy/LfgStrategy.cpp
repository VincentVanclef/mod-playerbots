/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgStrategy.h"

#include "Playerbots.h"

void LfgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Queue joins need to react much faster than the stock random cadence or
    // freshly logged-in RTG queue-fill bots can sit idle for many minutes before
    // they even attempt their first RDF join packet. The action itself already
    // self-throttles, so an extra often trigger is safe here.
    triggers.push_back(new TriggerNode("often", { NextAction("lfg join", relevance) }));
    triggers.push_back(new TriggerNode("random", { NextAction("lfg join", relevance) }));
    triggers.push_back(
        new TriggerNode("seldom", { NextAction("lfg leave", relevance) }));
    triggers.push_back(new TriggerNode(
        "unknown dungeon", { NextAction("give leader in dungeon", relevance) }));
}

LfgStrategy::LfgStrategy(PlayerbotAI* botAI) : PassTroughStrategy(botAI) {}
