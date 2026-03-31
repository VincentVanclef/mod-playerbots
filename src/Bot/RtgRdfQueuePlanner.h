#pragma once

#include "Define.h"

#include <map>

class RandomPlayerbotMgr;

namespace RTG
{
struct RtgLfgQueueOwnerSnapshot
{
    uint32 owner = 0;
    uint32 team = 0;
    uint32 level = 0;
    uint32 realTank = 0;
    uint32 realHeal = 0;
    uint32 realDps = 0;
    uint32 realQueued = 0;
    uint32 realActive = 0;
    uint32 helperQueuedTank = 0;
    uint32 helperQueuedHeal = 0;
    uint32 helperQueuedDps = 0;
    uint32 helperAssignedTank = 0;
    uint32 helperAssignedHeal = 0;
    uint32 helperAssignedDps = 0;
    bool activeDungeon = false;
};

class RtgRdfQueuePlanner
{
public:
    void ApplyDemandEvents(RandomPlayerbotMgr& mgr,
        std::map<uint32, RtgLfgQueueOwnerSnapshot> const& requests,
        bool anyRealLfgDemand) const;
};
}
