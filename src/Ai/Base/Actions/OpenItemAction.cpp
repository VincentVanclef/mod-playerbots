#include "OpenItemAction.h"
#include "PlayerbotAI.h"
#include "ItemTemplate.h"
#include "WorldPacket.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "LootObjectStack.h"
#include "AiObjectContext.h"

bool OpenItemAction::Execute(Event /*event*/)
{
    bool foundOpenable = false;

    Item* item = botAI->FindOpenableItem();
    if (item)
    {
        uint8 bag = item->GetBagSlot();  // Retrieves the bag slot (0 for main inventory)
        uint8 slot = item->GetSlot();    // Retrieves the actual slot inside the bag

        OpenItem(item, bag, slot);
        foundOpenable = true;
    }

    return foundOpenable;
}

void OpenItemAction::OpenItem(Item* item, uint8 bag, uint8 slot)
{
    if (!item)
    {
        botAI->TellError("Cannot open item: item not found");
        return;
    }

    if (!item->IsInWorld())
    {
        botAI->TellError("Cannot open item: item already removed");
        return;
    }

    const ObjectGuid guid = item->GetGUID();
    const std::string name = item->GetTemplate()->Name1;

    WorldPacket packet(CMSG_OPEN_ITEM);
    packet << bag << slot;
    bot->GetSession()->HandleOpenItemOpcode(packet);

    // Store the item GUID as the loot target
    LootObject lootObject;
    lootObject.guid = guid;
    botAI->GetAiObjectContext()->GetValue<LootObject>("loot target")->Set(lootObject);

    std::ostringstream out;
    out << "Opened item: " << name;
    botAI->TellMaster(out.str());
}
