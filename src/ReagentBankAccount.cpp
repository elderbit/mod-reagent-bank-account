#include "ReagentBankAccount.h"
#include <unordered_map>
#include <utility>
#include <sstream>
#include <mutex>
#include <optional>

uint32 g_maxOptionsPerPage;
bool g_accountWideReagentBank = false;
static bool g_reagentBankAudit = false;
static uint32 g_reagentBankAuditRetentionSeconds = 7 * DAY;
static uint32 g_reagentBankAuditCleanupIntervalSeconds = HOUR;
static time_t g_reagentBankLastCleanup = 0;

// AzerothCore module: Account-wide Reagent Bank
// This script adds a reagent bank NPC that allows players to deposit and
// withdraw reagents account-wide.

class mod_reagent_bank_account : public CreatureScript
{
private:
  // Caches for item templates and icons
  mutable std::unordered_map<uint32, const ItemTemplate *> itemTemplateCache;
  mutable std::unordered_map<uint32, std::string> itemIconCache;
  // Per-player last viewed category & page (guid low -> pair(category, page))
  mutable std::unordered_map<uint32, std::pair<uint32, uint16>> m_lastView;
  mutable std::mutex m_mutex; // protects caches & m_lastView
  // Cached category summaries (subclass -> (distinctItems, totalAmount))
  mutable std::unordered_map<uint32, std::pair<uint32, uint64>> m_categorySummaryCache;
  mutable bool m_cacheAllCategoriesInvalid = true;

  struct ReagentCategoryInfo
  {
    uint32 subclass;
    uint32 sampleIconItem; // representative item id for root menu icon
    const char *name;
  };

  static constexpr ReagentCategoryInfo kCategories[] = {
      {ITEM_SUBCLASS_CLOTH, 2589, "Cloth"},
      {ITEM_SUBCLASS_MEAT, 12208, "Meat"},
      {ITEM_SUBCLASS_METAL_STONE, 2772, "Metal & Stone"},
      {ITEM_SUBCLASS_ENCHANTING, 10940, "Enchanting"},
      {ITEM_SUBCLASS_ELEMENTAL, 7068, "Elemental"},
      {ITEM_SUBCLASS_PARTS, 4359, "Parts"},
      {ITEM_SUBCLASS_TRADE_GOODS_OTHER, 2604, "Other Trade Goods"},
      {ITEM_SUBCLASS_HERB, 2453, "Herb"},
      {ITEM_SUBCLASS_LEATHER, 2318, "Leather"},
      {ITEM_SUBCLASS_JEWELCRAFTING, 1206, "Jewelcrafting"},
      {ITEM_SUBCLASS_EXPLOSIVES, 4358, "Explosives"},
      {ITEM_SUBCLASS_DEVICES, 4388, "Devices"},
      {ITEM_SUBCLASS_MATERIAL, 23572, "Nether Material"},
      {ITEM_SUBCLASS_ARMOR_ENCHANTMENT, 38682, "Armor Vellum"},
      {ITEM_SUBCLASS_WEAPON_ENCHANTMENT, 39349, "Weapon Vellum"},
  };

  // Common icon item IDs (avoid scattered magic numbers)
  static constexpr uint32 ICON_DEPOSIT_WITHDRAW = 2901; // pick style icon
  static constexpr uint32 ICON_PAGINATION = 23705;      // arrow / nav icon
  static constexpr uint32 ICON_BACK = 6948;             // hearthstone style

  static const char *GetCategoryName(uint32 subclass)
  {
    for (auto const &c : kCategories)
      if (c.subclass == subclass)
        return c.name;
    return "Reagents";
  }

  // Helper: category identification via metadata array
  bool IsReagentCategory(uint32 value) const
  {
    for (auto const &c : kCategories)
      if (c.subclass == value)
        return true;
    return false;
  }

  // Invalidate one category or all (subclass==0 => all)
  void InvalidateCategorySummary(uint32 subclass)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (subclass == 0)
    {
      m_categorySummaryCache.clear();
      m_cacheAllCategoriesInvalid = true;
    }
    else
    {
      m_categorySummaryCache.erase(subclass);
    }
  }

  std::optional<std::pair<uint32, uint64>> GetCachedCategorySummary(uint32 subclass) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_categorySummaryCache.find(subclass);
    if (it != m_categorySummaryCache.end())
      return it->second;
    return std::nullopt;
  }

  void StoreCategorySummary(uint32 subclass, std::pair<uint32, uint64> summary) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_categorySummaryCache[subclass] = summary;
    m_cacheAllCategoriesInvalid = false;
  }

  // Helper: resolve storage keys (account_id, guid) according to mode
  std::pair<uint32, uint32> GetStorageKeys(Player *player) const
  {
    uint32 accountId = player->GetSession()->GetAccountId();
    uint32 guid = g_accountWideReagentBank ? 0 : player->GetGUID().GetRawValue();
    return {accountId, guid};
  }

  // Get and cache ItemTemplate
  const ItemTemplate *GetCachedItemTemplate(uint32 entry) const
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = itemTemplateCache.find(entry);
      if (it != itemTemplateCache.end())
        return it->second;
    }
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      itemTemplateCache[entry] = temp;
    }
    return temp;
  }

  // Get and cache item icon string
  std::string GetCachedItemIcon(uint32 entry, uint32 width, uint32 height,
                                int x, int y) const
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = itemIconCache.find(entry);
      if (it != itemIconCache.end())
        return it->second;
    }
    std::ostringstream ss;
    ss << "|TInterface";
    const ItemTemplate *temp = GetCachedItemTemplate(entry);
    const ItemDisplayInfoEntry *dispInfo = nullptr;
    if (temp)
    {
      dispInfo = sItemDisplayInfoStore.LookupEntry(temp->DisplayInfoID);
      if (dispInfo)
        ss << "/ICONS/" << dispInfo->inventoryIcon;
    }
    if (!dispInfo)
      ss << "/InventoryItems/WoWUnknownItem01";
    ss << ":" << width << ":" << height << ":" << x << ":" << y << "|t";
    std::string iconStr = ss.str();
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      itemIconCache[entry] = iconStr;
    }
    return iconStr;
  }

  // Returns a colored item link string for display in gossip menus (no cache,
  // as it may be locale-dependent)
  std::string GetItemLink(uint32 entry, WorldSession *session) const
  {
    int loc_idx = session->GetSessionDbLocaleIndex();
    const ItemTemplate *temp = GetCachedItemTemplate(entry);
    std::string name = temp ? temp->Name1 : "Unknown";
    if (temp)
    {
      if (ItemLocale const *il = sObjectMgr->GetItemLocale(temp->ItemId))
        ObjectMgr::GetLocaleString(il->Name, loc_idx, name);
    }
    std::ostringstream oss;
    oss << "|c";
    if (temp)
      oss << std::hex << ItemQualityColors[temp->Quality] << std::dec;
    else
      oss << "ffffffff";
    oss << "|Hitem:" << entry << ":0|h[" << name << "]|h|r";
    return oss.str();
  }

  // Async single-item withdraw (reduces sync DB stall)
  void WithdrawItem(Player *player, uint32 entry)
  {
    auto [accountId, guid] = GetStorageKeys(player);
    WorldSession *session = player->GetSession();
    ObjectGuid playerGuid = player->GetGUID();
    std::string q = "SELECT amount FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountId) + " AND guid = " + std::to_string(guid) + " AND item_entry = " + std::to_string(entry);
    session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(q).WithCallback([=, this](QueryResult result)
                                                                                          {
      Player* p = ObjectAccessor::FindPlayer(playerGuid);
      if (!p || !p->GetSession()) return;
      if (!result)
      {
        ChatHandler(p->GetSession()).PSendSysMessage("No stored reagents for item %u.", entry);
        return;
      }
      uint32 storedAmount = (*result)[0].Get<uint32>();
      const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
      if (!temp)
      {
        ChatHandler(p->GetSession()).PSendSysMessage("Error: Item template not found for entry %u.", entry);
        return;
      }
      uint32 stackSize = temp->GetMaxStackSize();
      if (storedAmount <= stackSize)
      {
        ItemPosCountVec dest; InventoryResult msg = p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, storedAmount);
        if (msg == EQUIP_ERR_OK)
        {
          CharacterDatabase.Execute("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountId, guid, entry);
          if (Item* item = p->StoreNewItem(dest, entry, true)) p->SendNewItem(item, storedAmount, true, false);
          ChatHandler(p->GetSession()).PSendSysMessage("Withdrew %u x %s.", storedAmount, temp->Name1.c_str());
              // Invalidate only that item's subclass summary (need subclass lookup)
              InvalidateCategorySummary(temp->SubClass);
        }
        else
        {
          p->SendEquipError(msg, nullptr, nullptr, entry);
          ChatHandler(p->GetSession()).PSendSysMessage("Not enough bag space to withdraw %u x %s.", storedAmount, temp->Name1.c_str());
        }
      }
      else
      {
        ItemPosCountVec dest; InventoryResult msg = p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, stackSize);
        if (msg == EQUIP_ERR_OK)
        {
          CharacterDatabase.Execute("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", storedAmount - stackSize, accountId, guid, entry);
            if (Item* item = p->StoreNewItem(dest, entry, true)) p->SendNewItem(item, stackSize, true, false);
            ChatHandler(p->GetSession()).PSendSysMessage("Withdrew %u x %s.", stackSize, temp->Name1.c_str());
                InvalidateCategorySummary(temp->SubClass);
        }
        else
        {
          p->SendEquipError(msg, nullptr, nullptr, entry);
          ChatHandler(p->GetSession()).PSendSysMessage("Not enough bag space to withdraw %u x %s.", stackSize, temp->Name1.c_str());
        }
      } }));
  }

  // Dry-run capacity simulator (approximate). Returns entry->canWithdrawAmount.
  std::map<uint32, uint32> SimulateBatchAdd(Player *player, const std::vector<std::pair<uint32, uint32>> &items)
  {
    // Build mutable snapshot of current bag free space and partial stacks.
    struct StackSlot
    {
      uint32 freeSpace;
      Item *item;
    };
    // For each item entry, collect partial stacks (freeSpace > 0)
    std::unordered_map<uint32, std::vector<StackSlot>> partial;
    std::unordered_map<uint32, uint32> emptySlotsByFamily; // bagFamilyMask -> count (0 = generic)
    // Scan backpack
    auto scanBag = [&](uint8 bagPos, Bag *bag, uint32 size)
    {
      uint32 familyMask = bag ? bag->GetBagFamily() : 0;
      for (uint32 slot = 0; slot < size; ++slot)
      {
        Item *it = player->GetItemByPos(bagPos, slot);
        if (!it)
        {
          emptySlotsByFamily[familyMask]++;
          continue;
        }
        ItemTemplate const *tmpl = it->GetTemplate();
        if (!tmpl)
          continue;
        uint32 maxStack = tmpl->GetMaxStackSize();
        if (maxStack <= 1)
          continue; // ignore non-stackables for simulation
        uint32 count = it->GetCount();
        if (count < maxStack)
        {
          partial[tmpl->ItemId].push_back({maxStack - count, it});
        }
      }
    };
    // Backpack
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
      Item *it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
      if (!it)
      {
        emptySlotsByFamily[0]++;
        continue;
      }
      ItemTemplate const *tmpl = it->GetTemplate();
      if (!tmpl)
        continue;
      uint32 maxStack = tmpl->GetMaxStackSize();
      if (maxStack <= 1)
        continue;
      uint32 count = it->GetCount();
      if (count < maxStack)
        partial[tmpl->ItemId].push_back({maxStack - count, it});
    }
    // Other bags
    for (uint8 bagPos = INVENTORY_SLOT_BAG_START; bagPos < INVENTORY_SLOT_BAG_END; ++bagPos)
    {
      if (Bag *bag = player->GetBagByPos(bagPos))
        scanBag(bagPos, bag, bag->GetBagSize());
    }
    std::map<uint32, uint32> granted; // result map
    // Process each requested item deterministically
    for (auto const &pr : items)
    {
      uint32 entry = pr.first;
      uint32 remaining = pr.second;
      const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
      if (!temp)
        continue;
      uint32 maxStack = temp->GetMaxStackSize();
      if (maxStack == 0)
        continue;
      // Fill partial stacks first
      auto &vec = partial[entry];
      for (auto &slot : vec)
      {
        if (remaining == 0)
          break;
        uint32 use = std::min(slot.freeSpace, remaining);
        slot.freeSpace -= use;
        remaining -= use;
        granted[entry] += use;
      }
      // Remove exhausted partial slots
      vec.erase(std::remove_if(vec.begin(), vec.end(), [](StackSlot const &s)
                               { return s.freeSpace == 0; }),
                vec.end());
      // Use empty slots to create new stacks
      auto fitsSpecialty = [&](uint32 bagMask)
      { return bagMask == 0 || (temp->GetBagFamily() & bagMask) != 0; };
      while (remaining > 0)
      {
        bool placed = false;
        // Try specialty first (non-zero masks)
        for (auto &kv : emptySlotsByFamily)
        {
          if (kv.first == 0 || kv.second == 0)
            continue;
          if (!fitsSpecialty(kv.first))
            continue;
          uint32 create = std::min(maxStack, remaining);
          remaining -= create;
          granted[entry] += create;
          kv.second--;
          placed = true;
          break;
        }
        if (!placed)
        {
          auto itEmpty = emptySlotsByFamily.find(0);
          if (itEmpty == emptySlotsByFamily.end() || itEmpty->second == 0)
            break; // no slot
          uint32 create = std::min(maxStack, remaining);
          remaining -= create;
          granted[entry] += create;
          itEmpty->second--;
          placed = true;
        }
        if (!placed)
          break;
      }
      // Any leftover remaining cannot be stored.
    }
    return granted;
  }

  // Fetch or build category summary
  std::pair<uint32, uint64> GetCategorySummary(uint32 subclass, uint32 accountId, uint32 guid)
  {
    if (auto cached = GetCachedCategorySummary(subclass))
      return *cached;
    std::string q = "SELECT COUNT(*), COALESCE(SUM(amount),0) FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountId) + " AND guid = " + std::to_string(guid) + " AND item_subclass = " + std::to_string(subclass);
    if (QueryResult result = CharacterDatabase.Query(q))
    {
      uint32 distinctCount = (*result)[0].Get<uint32>();
      uint64 totalAmount = (*result)[1].Get<uint64>();
      StoreCategorySummary(subclass, {distinctCount, totalAmount});
      return {distinctCount, totalAmount};
    }
    StoreCategorySummary(subclass, {0, 0});
    return {0, 0};
  }

  // Updates the item count maps and removes the item from the player's
  // inventory
  struct AccumulateResult
  {
    std::map<uint32, uint32> addedCounts;     // counts just removed from bags
    std::map<uint32, uint32> subclassByEntry; // subclass for each item
  };

  // Iterate player inventory & bags, collect reagent items (optionally filtered by subclass), destroy them, and accumulate counts.
  AccumulateResult AccumulateInventory(Player *player, std::optional<uint32> filterSubclass)
  {
    AccumulateResult result;

    auto considerItem = [&](Item *pItem, uint32 bagSlot, uint32 itemSlot)
    {
      if (!pItem)
        return;
      ItemTemplate const *itemTemplate = pItem->GetTemplate();
      if (!itemTemplate)
        return;
      // Only allow trade goods and gems, skip unique/stack size 1
      if (!((itemTemplate->Class == ITEM_CLASS_TRADE_GOODS) || (itemTemplate->Class == ITEM_CLASS_GEM)))
        return;
      if (itemTemplate->GetMaxStackSize() == 1)
        return;

      uint32 subclass = (itemTemplate->Class == ITEM_CLASS_GEM) ? ITEM_SUBCLASS_JEWELCRAFTING : itemTemplate->SubClass;
      if (filterSubclass && subclass != *filterSubclass)
        return; // filtered out

      uint32 entry = itemTemplate->ItemId;
      uint32 count = pItem->GetCount();

      result.addedCounts[entry] += count;
      // only set subclass once
      if (!result.subclassByEntry.count(entry))
        result.subclassByEntry[entry] = subclass;

      player->DestroyItem(bagSlot, itemSlot, true); // remove from inventory
    };

    // Inventory (backpack) slots
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
      considerItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, i), INVENTORY_SLOT_BAG_0, i);

    // Additional bags
    for (uint8 bagPos = INVENTORY_SLOT_BAG_START; bagPos < INVENTORY_SLOT_BAG_END; ++bagPos)
    {
      if (Bag *bag = player->GetBagByPos(bagPos))
      {
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
          considerItem(player->GetItemByPos(bagPos, slot), bagPos, slot);
      }
    }

    return result;
  }

  // Flush merged reagent state to DB using INSERT ... ON DUPLICATE KEY UPDATE (avoids DELETE+INSERT of REPLACE)
  void FlushReagentState(uint32 accountId, uint32 guid, const std::map<uint32, uint32> &finalAmounts, const std::map<uint32, uint32> &subclassByEntry)
  {
    if (finalAmounts.empty())
      return;
    auto trans = CharacterDatabase.BeginTransaction();
    for (auto const &kv : finalAmounts)
    {
      uint32 entry = kv.first;
      uint32 amount = kv.second;
      auto it = subclassByEntry.find(entry);
      uint32 subclass = (it != subclassByEntry.end()) ? it->second : 0;
      trans->Append("INSERT INTO mod_reagent_bank_account (account_id, guid, item_entry, item_subclass, amount) VALUES ({}, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE item_subclass=VALUES(item_subclass), amount=VALUES(amount)", accountId, guid, entry, subclass, amount);
    }
    CharacterDatabase.CommitTransaction(trans);
  }

  // Deposits all reagents from the player's bags into the account-wide bank
  void DepositAllReagents(Player *player)
  {
    WorldSession *session = player->GetSession();
    auto [accountId, guid] = GetStorageKeys(player);
    std::string query = "SELECT item_entry, item_subclass, amount FROM mod_reagent_bank_account WHERE account_id = " +
                        std::to_string(accountId) + " AND guid = " + std::to_string(guid);
    ObjectGuid playerGuid = player->GetGUID();
    session->GetQueryProcessor().AddCallback(
        CharacterDatabase.AsyncQuery(query).WithCallback([=, this](QueryResult result)
                                                         {
              Player* playerPtr = ObjectAccessor::FindPlayer(playerGuid);
              if (!playerPtr || !playerPtr->GetSession())
                return; // player logged out
              auto [accountId2, guid2] = GetStorageKeys(playerPtr);

              // Load existing amounts
              std::map<uint32, uint32> existingAmounts; // itemEntry -> amount
              std::map<uint32, uint32> subclassByEntry; // from DB (for safety)
              if (result)
              {
                do
                {
                  uint32 itemEntry = (*result)[0].Get<uint32>();
                  uint32 itemSubclass = (*result)[1].Get<uint32>();
                  uint32 itemAmount = (*result)[2].Get<uint32>();
                  existingAmounts[itemEntry] = itemAmount;
                  subclassByEntry[itemEntry] = itemSubclass;
                } while (result->NextRow());
              }

              // Accumulate new deposits from inventory (all subclasses)
              AccumulateResult accum = AccumulateInventory(playerPtr, std::nullopt);

              if (!accum.addedCounts.empty())
              {
                TC_LOG_DEBUG("misc", "ReagentBank Deposit logical batch account=%u guid=%u newEntries=%zu", accountId2, guid2, accum.addedCounts.size());
                if (g_reagentBankAudit)
                {
                  auto audit = CharacterDatabase.BeginTransaction();
                  for (auto const& kv : accum.addedCounts)
                  {
                    uint32 entryId = kv.first; uint32 delta = kv.second; uint32 subclass = accum.subclassByEntry.at(entryId);
                    audit->Append("INSERT INTO mod_reagent_bank_audit (ts, account_id, guid, action, item_entry, item_subclass, delta) VALUES (UNIX_TIMESTAMP(), {}, {}, 'DEPOSIT', {}, {}, {})", accountId2, guid2, entryId, subclass, delta);
                  }
                  CharacterDatabase.CommitTransaction(audit);
                }
                // Merge existing + new into final map
                std::map<uint32, uint32> finalAmounts = existingAmounts;
                for (auto const &kv : accum.addedCounts)
                  finalAmounts[kv.first] = existingAmounts[kv.first] + kv.second;

                // Build complete subclass map (prefer new subclass info)
                std::map<uint32, uint32> mergedSubclass = subclassByEntry;
                for (auto const &kv : accum.subclassByEntry)
                  mergedSubclass[kv.first] = kv.second;

                FlushReagentState(accountId2, guid2, finalAmounts, mergedSubclass);
                // Invalidate only subclasses touched
                std::set<uint32> touched;
                for (auto const& kv : accum.subclassByEntry) touched.insert(kv.second);
                for (auto sc : touched) InvalidateCategorySummary(sc);

                ChatHandler(playerPtr->GetSession()).SendSysMessage("The following was deposited:");
                for (auto const &kv : accum.addedCounts)
                {
                  uint32 itemEntry = kv.first;
                  uint32 added = kv.second;
                  if (ItemTemplate const *itemTemplate = sObjectMgr->GetItemTemplate(itemEntry))
                    ChatHandler(playerPtr->GetSession()).SendSysMessage(std::to_string(added) + " " + itemTemplate->Name1);
                }
              }
              else
              {
                ChatHandler(playerPtr->GetSession()).PSendSysMessage("No reagents to deposit.");
              } }));

    CloseGossipMenuFor(player);
  }

  void DepositAllReagentsForCategory(Player *player, uint32 item_subclass)
  {
    WorldSession *session = player->GetSession();
    auto [accountId, guid] = GetStorageKeys(player);
    std::string query = "SELECT item_entry, amount FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountId) + " AND guid = " + std::to_string(guid) + " AND item_subclass = " + std::to_string(item_subclass);
    ObjectGuid playerGuid = player->GetGUID();
    session->GetQueryProcessor().AddCallback(
        CharacterDatabase.AsyncQuery(query).WithCallback([=, this](QueryResult result)
                                                         {
              Player* playerPtr = ObjectAccessor::FindPlayer(playerGuid);
              if (!playerPtr || !playerPtr->GetSession())
                return;
              auto [accountId2, guid2] = GetStorageKeys(playerPtr);
              std::map<uint32, uint32> existingAmounts;
              if (result)
              {
                do
                {
                  uint32 entry = (*result)[0].Get<uint32>();
                  uint32 amount = (*result)[1].Get<uint32>();
                  existingAmounts[entry] = amount;
                } while (result->NextRow());
              }

              AccumulateResult accum = AccumulateInventory(playerPtr, item_subclass);

              if (!accum.addedCounts.empty())
              {
                TC_LOG_DEBUG("misc", "ReagentBank DepositCategory logical batch account=%u guid=%u subclass=%u newEntries=%zu", accountId2, guid2, item_subclass, accum.addedCounts.size());
                if (g_reagentBankAudit)
                {
                  auto audit = CharacterDatabase.BeginTransaction();
                  for (auto const& kv : accum.addedCounts)
                  {
                    uint32 entryId = kv.first; uint32 delta = kv.second;
                    audit->Append("INSERT INTO mod_reagent_bank_audit (ts, account_id, guid, action, item_entry, item_subclass, delta) VALUES (UNIX_TIMESTAMP(), {}, {}, 'DEPOSIT', {}, {}, {})", accountId2, guid2, entryId, item_subclass, delta);
                  }
                  CharacterDatabase.CommitTransaction(audit);
                }
                std::map<uint32, uint32> finalAmounts = existingAmounts;
                for (auto const &kv : accum.addedCounts)
                  finalAmounts[kv.first] = existingAmounts[kv.first] + kv.second;
                // subclass map is uniform for this category
                std::map<uint32, uint32> subclassMap;
                for (auto const &kv : finalAmounts)
                  subclassMap[kv.first] = item_subclass;
                FlushReagentState(accountId2, guid2, finalAmounts, subclassMap);
                InvalidateCategorySummary(item_subclass);

                ChatHandler(playerPtr->GetSession()).SendSysMessage("The following was deposited:");
                for (auto const &kv : accum.addedCounts)
                {
                  uint32 itemEntry = kv.first;
                  uint32 added = kv.second;
                  if (ItemTemplate const *itemTemplate = sObjectMgr->GetItemTemplate(itemEntry))
                    ChatHandler(playerPtr->GetSession()).SendSysMessage(std::to_string(added) + " " + itemTemplate->Name1);
                }
              }
              else
              {
                ChatHandler(playerPtr->GetSession()).PSendSysMessage("No reagents to deposit in this category.");
              } }));

    CloseGossipMenuFor(player);
  }

  // Helper: Withdraw all items in a category for the player
  void WithdrawAllInCategory(Player *player, uint32 item_subclass)
  {
    auto [accountId, guid] = GetStorageKeys(player);
    WorldSession *session = player->GetSession();
    ObjectGuid playerGuid = player->GetGUID();
    std::string q = "SELECT item_entry, amount FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountId) + " AND guid = " + std::to_string(guid) + " AND item_subclass = " + std::to_string(item_subclass);
    session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(q).WithCallback([=, this](QueryResult result)
                                                                                          {
      Player* p = ObjectAccessor::FindPlayer(playerGuid);
      if (!p || !p->GetSession()) return;
      if (!result)
      { ChatHandler(p->GetSession()).PSendSysMessage("No reagents to withdraw in this category."); return; }
      // Copy rows to vector for simulation
      std::vector<std::pair<uint32,uint32>> items;
      do {
        uint32 itemEntry = (*result)[0].Get<uint32>();
        uint32 amount = (*result)[1].Get<uint32>();
        items.emplace_back(itemEntry, amount);
      } while (result->NextRow());
      auto sim = SimulateBatchAdd(p, items);
      bool any = false;
      for (auto const& pr : items)
      {
        uint32 itemEntry = pr.first; uint32 amount = pr.second;
        uint32 allowed = sim[itemEntry];
        if (allowed == 0) continue;
        const ItemTemplate* temp = sObjectMgr->GetItemTemplate(itemEntry);
        if (!temp) continue;
        uint32 stackSize = temp->GetMaxStackSize();
        uint32 remainingToGive = allowed;
        uint32 remainingBank = amount;
        while (remainingToGive > 0)
        {
          uint32 toGive = std::min(stackSize, remainingToGive);
          ItemPosCountVec dest; InventoryResult msg = p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, toGive);
          if (msg != EQUIP_ERR_OK) { p->SendEquipError(msg, nullptr, nullptr, itemEntry); break; }
          if (Item* newItem = p->StoreNewItem(dest, itemEntry, true)) p->SendNewItem(newItem, toGive, true, false);
          ChatHandler(p->GetSession()).PSendSysMessage("Withdrew %u x %s.", toGive, temp->Name1.c_str());
          any = true;
          remainingToGive -= toGive;
          remainingBank -= toGive;
        }
        if (remainingBank == 0)
          CharacterDatabase.Execute("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountId, guid, itemEntry);
        else if (remainingBank != amount)
          CharacterDatabase.Execute("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", remainingBank, accountId, guid, itemEntry);
        if (g_reagentBankAudit && allowed > 0)
        {
          CharacterDatabase.Execute("INSERT INTO mod_reagent_bank_audit (ts, account_id, guid, action, item_entry, item_subclass, delta) VALUES (UNIX_TIMESTAMP(), {}, {}, 'WITHDRAW', {}, {}, {})", accountId, guid, itemEntry, item_subclass, allowed);
        }
      }
      if (!any)
        ChatHandler(p->GetSession()).PSendSysMessage("No reagents withdrawn (bag space).");
      else
      {
        // Batch DB updates & audit inside one transaction
        auto trans = CharacterDatabase.BeginTransaction();
        for (auto const& pr : items)
        {
          uint32 itemEntry = pr.first; uint32 amount = pr.second; uint32 allowed = sim[itemEntry];
          if (allowed == 0) continue; // no change
          uint32 remainingBank = (amount > allowed) ? (amount - allowed) : 0;
          if (remainingBank == 0)
            trans->Append("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountId, guid, itemEntry);
          else if (remainingBank != amount)
            trans->Append("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", remainingBank, accountId, guid, itemEntry);
          if (g_reagentBankAudit && allowed > 0)
            trans->Append("INSERT INTO mod_reagent_bank_audit (ts, account_id, guid, action, item_entry, item_subclass, delta) VALUES (UNIX_TIMESTAMP(), {}, {}, 'WITHDRAW', {}, {}, {})", accountId, guid, itemEntry, item_subclass, allowed);
        }
        CharacterDatabase.CommitTransaction(trans);
  if (g_reagentBankAudit) EnsureAuditCleanup();
  InvalidateCategorySummary(item_subclass);
      } }));
  }

public:
  // Constructor: reads config for max options per page
  mod_reagent_bank_account() : CreatureScript("mod_reagent_bank_account")
  {
    g_maxOptionsPerPage = sConfigMgr->GetOption<uint32>(
        "ReagentBankAccount.MaxOptionsPerPage", DEFAULT_MAX_OPTIONS);
    g_accountWideReagentBank =
        sConfigMgr->GetOption<bool>("ReagentBankAccount.AccountWide", false);
    g_reagentBankAudit = sConfigMgr->GetOption<bool>("ReagentBankAccount.Audit", false);
    g_reagentBankAuditRetentionSeconds = sConfigMgr->GetOption<uint32>("ReagentBankAccount.AuditRetentionSeconds", g_reagentBankAuditRetentionSeconds);
    g_reagentBankAuditCleanupIntervalSeconds = sConfigMgr->GetOption<uint32>("ReagentBankAccount.AuditCleanupIntervalSeconds", g_reagentBankAuditCleanupIntervalSeconds);
  }

  void EnsureAuditCleanup()
  {
    if (!g_reagentBankAudit)
      return;
    time_t now = GameTime::GetGameTime();
    if (g_reagentBankLastCleanup && (now - g_reagentBankLastCleanup) < g_reagentBankAuditCleanupIntervalSeconds)
      return;
    g_reagentBankLastCleanup = now;
    time_t cutoff = now - g_reagentBankAuditRetentionSeconds;
    CharacterDatabase.AsyncPQuery("DELETE FROM mod_reagent_bank_audit WHERE ts < {}", cutoff);
  }

  // Main menu for the reagent banker NPC
  bool OnGossipHello(Player *player, Creature *creature) override
  {
    constexpr int MAIN_ICON_SIZE = 24;
    constexpr int MAIN_ICON_X = 0;
    constexpr int MAIN_ICON_Y = 0;
    constexpr int GOSSIP_ICON_NONE = 0;
    player->PlayerTalkClass->ClearMenus();
    AddGossipItemFor(player, GOSSIP_ICON_NONE, "Deposit All Reagents", DEPOSIT_ALL_REAGENTS, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE, "Withdraw All Reagents", WITHDRAW_ALL_REAGENTS, 0);
    auto [accountId, guid] = GetStorageKeys(player);
    // Async aggregate query for all categories
    std::ostringstream qs;
    qs << "SELECT item_subclass, COUNT(*), COALESCE(SUM(amount),0) FROM mod_reagent_bank_account WHERE account_id=" << accountId << " AND guid=" << guid << " GROUP BY item_subclass";
    ObjectGuid pg = player->GetGUID();
    WorldSession *session = player->GetSession();
    session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(qs.str()).WithCallback([=, this](QueryResult result)
                                                                                                 {
      Player* pl = ObjectAccessor::FindPlayer(pg); if (!pl || !pl->GetSession()) return; pl->PlayerTalkClass->ClearMenus();
      AddGossipItemFor(pl, GOSSIP_ICON_NONE, "Deposit All Reagents", DEPOSIT_ALL_REAGENTS, 0);
      AddGossipItemFor(pl, GOSSIP_ICON_NONE, "Withdraw All Reagents", WITHDRAW_ALL_REAGENTS, 0);
      std::unordered_map<uint32, std::pair<uint32,uint64>> fresh;
      if (result)
      {
        do {
          uint32 sc = (*result)[0].Get<uint32>();
          uint32 distinct = (*result)[1].Get<uint32>();
          uint64 total = (*result)[2].Get<uint64>();
          fresh[sc] = {distinct, total};
        } while (result->NextRow());
      }
      // Store into cache
      for (auto const& kv : fresh) StoreCategorySummary(kv.first, kv.second);
      // Build menu using cache (zero default for missing)
      for (auto const &info : kCategories)
      {
        auto cached = GetCachedCategorySummary(info.subclass);
        uint32 distinctItems = cached ? cached->first : 0; uint64 totalAmount = cached ? cached->second : 0;
        AddGossipItemFor(pl, GOSSIP_ICON_NONE,
          GetCachedItemIcon(info.sampleIconItem, MAIN_ICON_SIZE, MAIN_ICON_SIZE, MAIN_ICON_X, MAIN_ICON_Y) + std::string(info.name) +
          " |cff000000(" + std::to_string(distinctItems) + "/" + std::to_string(totalAmount) + ")|r",
          info.subclass, 0);
      }
      SendGossipMenuFor(pl, NPC_TEXT_ID, pl->GetGUID()); }));
    return true;
  }

  // Handles menu selections and confirmation dialogs
  bool OnGossipSelect(Player *player, Creature *creature, uint32 item_subclass,
                      uint32 gossipPageNumber) override
  {
    player->PlayerTalkClass->ClearMenus();

    if (item_subclass == DEPOSIT_ALL_REAGENTS)
    {
      if (gossipPageNumber == 0)
      {
        // Main menu: deposit all categories
        DepositAllReagents(player);
      }
      else
      {
        // Category menu: deposit only this category
        DepositAllReagentsForCategory(player, gossipPageNumber);
      }
      return true;
    }
    else if (item_subclass == WITHDRAW_ALL_REAGENTS)
    {
      if (gossipPageNumber == 0)
      {
        // Main menu: withdraw all categories
        for (auto const &info : kCategories)
          WithdrawAllInCategory(player, info.subclass);
      }
      else
      {
        // Category menu: withdraw only this category
        WithdrawAllInCategory(player, gossipPageNumber);
      }
      CloseGossipMenuFor(player);
      return true;
    }
    else if (item_subclass == MAIN_MENU)
    {
      OnGossipHello(player, creature);
      return true;
    }
    else
    {
      // If this is a category, show items. Otherwise treat as item entry withdrawal.
      if (IsReagentCategory(item_subclass))
      {
        ShowReagentItems(player, creature, item_subclass, gossipPageNumber);
        return true;
      }
      // Withdraw single item entry (item_subclass actually holds itemEntry in this path)
      WithdrawItem(player, item_subclass);
      uint32 guidLow = player->GetGUID().GetCounter();
      uint32 cat = 0;
      uint16 page = 0;
      bool haveContext = false;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_lastView.find(guidLow);
        if (it != m_lastView.end())
        {
          cat = it->second.first;
          page = it->second.second;
          haveContext = true;
        }
      }
      if (haveContext)
        ShowReagentItems(player, creature, cat, page);
      else
        OnGossipHello(player, creature);
      return true;
    }
  }

  // Shows the list of stored reagents for a category, with pagination
  void ShowReagentItems(Player *player, Creature *creature,
                        uint32 item_subclass, uint16 gossipPageNumber)
  {
    // Remember context for refresh after item withdrawal
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_lastView[player->GetGUID().GetCounter()] = {item_subclass, gossipPageNumber};
    }
    WorldSession *session = player->GetSession();
    auto [accountId, guid] = GetStorageKeys(player);
    std::string query = "SELECT item_entry, amount FROM mod_reagent_bank_account WHERE account_id = " +
                        std::to_string(accountId) + " AND guid = " + std::to_string(guid) +
                        " AND item_subclass = " + std::to_string(item_subclass) +
                        " ORDER BY item_entry DESC";
    ObjectGuid playerGuid = player->GetGUID();
    session->GetQueryProcessor().AddCallback(
        CharacterDatabase.AsyncQuery(query).WithCallback([=, this](QueryResult result)
                                                         {
              Player* playerPtr = ObjectAccessor::FindPlayer(playerGuid);
              if (!playerPtr || !playerPtr->GetSession())
                return;
              struct PageInfo { uint32 start; uint32 end; uint32 totalPages; uint32 currentPage; };
              auto CalcPage = [&](uint32 totalItems, uint16 page)->PageInfo {
                PageInfo p; p.currentPage = page + 1; // 1-based display
                p.totalPages = (totalItems == 0) ? 1 : ((totalItems - 1) / g_maxOptionsPerPage) + 1;
                p.start = page * g_maxOptionsPerPage;
                p.end = std::min<uint32>(p.start + g_maxOptionsPerPage - 1, (totalItems==0)?0:(totalItems-1));
                return p; };
              std::map<uint32, uint32> entryToAmountMap;
              std::vector<uint32> itemEntries;
              uint32 totalAmount = 0;
              if (result)
              {
                do
                {
                  uint32 itemEntry = (*result)[0].Get<uint32>();
                  uint32 itemAmount = (*result)[1].Get<uint32>();
                  entryToAmountMap[itemEntry] = itemAmount;
                  itemEntries.push_back(itemEntry);
                  totalAmount += itemAmount;
                } while (result->NextRow());
              }

              uint32 totalItems = itemEntries.size();
              PageInfo pageInfo = CalcPage(totalItems, gossipPageNumber);

              // --- Category summary at the top ---
              std::string categoryName = GetCategoryName(item_subclass);
              // Consistent icon size and offset
              constexpr int ICON_SIZE = 18;
              constexpr int ICON_X = 0;
              constexpr int ICON_Y = 0;
              constexpr int GOSSIP_ICON_NONE = 0;

              AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                               "|cff003366" + categoryName + ": " +
                                   std::to_string(totalItems) + " types, " + std::to_string(totalAmount) + " total|r",
                               0, 0);

              // Deposit All button (bold green, consistent icon)
              AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                               GetCachedItemIcon(ICON_DEPOSIT_WITHDRAW, ICON_SIZE, ICON_SIZE,
                                                 ICON_X, ICON_Y) + " |cff1eff00Deposit All|r",
                               DEPOSIT_ALL_REAGENTS, item_subclass);

              // Withdraw All button (bold blue, consistent icon)
              AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                               GetCachedItemIcon(ICON_DEPOSIT_WITHDRAW, ICON_SIZE, ICON_SIZE,
                                                 ICON_X, ICON_Y) + " |cff0070ddWithdraw All|r",
                               WITHDRAW_ALL_REAGENTS, item_subclass);

              // Pagination controls (dark blue, consistent icon)
              if (pageInfo.end + 1 < entryToAmountMap.size())
              {
        AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                 GetCachedItemIcon(ICON_PAGINATION, ICON_SIZE, ICON_SIZE,
                           ICON_X, ICON_Y) + " |cff003366Next Page|r ▶ (" +
                   std::to_string(pageInfo.currentPage + 1) + "/" + std::to_string(pageInfo.totalPages) + ")",
                 item_subclass, gossipPageNumber + 1);
              }
              if (gossipPageNumber > 0 && pageInfo.currentPage <= pageInfo.totalPages)
              {
        AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                 "◀ |cff003366Previous Page|r " +
                   GetCachedItemIcon(ICON_PAGINATION, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) +
                   " (" + std::to_string(pageInfo.currentPage - 1) + "/" + std::to_string(pageInfo.totalPages) + ")",
                 item_subclass, gossipPageNumber - 1);
              }

              // List items for this page with icon, colored link, and count in
              // black
              for (uint32 i = pageInfo.start; i <= pageInfo.end; ++i)
              {
                if (itemEntries.empty() || i > itemEntries.size() - 1)
                  break;
                uint32 itemEntry = itemEntries.at(i);
                uint32 amount = entryToAmountMap.find(itemEntry)->second;
                std::string link = GetItemLink(itemEntry, session);

                // Consistent icon size for items
                std::string icon = GetCachedItemIcon(itemEntry, ICON_SIZE,
                                                     ICON_SIZE, ICON_X, ICON_Y);

                // Compact display: [icon][Item Name] x amount (amount in black)
                AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                                 icon + link + " |cff000000x " +
                                     std::to_string(amount) + "|r",
                                 itemEntry, gossipPageNumber);
              }

              // Back button to main menu (gray, consistent icon)
              AddGossipItemFor(playerPtr, GOSSIP_ICON_NONE,
                               GetCachedItemIcon(ICON_BACK, ICON_SIZE, ICON_SIZE,
                                                 ICON_X, ICON_Y) + " |cff666666Back to Categories|r",
                               MAIN_MENU, 0);

              SendGossipMenuFor(playerPtr, NPC_TEXT_ID, creature->GetGUID()); }));
  }
};

// Add all scripts in one
void AddSC_mod_reagent_bank_account() { new mod_reagent_bank_account(); }
