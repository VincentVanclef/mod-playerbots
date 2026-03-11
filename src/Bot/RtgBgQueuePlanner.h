#pragma once

class RandomPlayerbotMgr;

namespace RTG
{
class RtgBgQueuePlanner
{
public:
    void ApplyDemandEvents(RandomPlayerbotMgr& mgr) const;
};
}
