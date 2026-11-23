#define BOOST_ALL_NO_LIB
#define BOOST_NO_CXX11_ALLOCATOR
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <kenshi/Character.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/GameData.h>
#include <kenshi/Enums.h>
#include <kenshi/util/YesNoMaybe.h>
#include <kenshi/util/lektor.h>
#include <core/Functions.h>
#include <KenshiLua.h>

class CraftingItem
{
public:
    Item* item;                 // 0x0
    float endTime;              // 0x8
    float progress01;           // 0xC
    hand smithBench;            // 0x10
    float rate;                 // 0x30
    std::string name;           // 0x38
    int lastErrorCode;          // 0x60
    YesNoMaybe criticalSuccess; // 0x64
};

class CraftingBuilding
{
public:
    CraftingItem* _addCraft(GameData* basedata, GameData* matdata, float progress, YesNoMaybe crit);
    void _NV_operate(Character* stats, float _amount);
};

class BeltBagInventoryGUI : public InventoryGUI
{
public:
    static BeltBagInventoryGUI* Create(Inventory* inv, InventoryLayout* layout, RootObject* callback)
    {
        void* mem = ::operator new(sizeof(InventoryGUI));
        BeltBagInventoryGUI* gui = static_cast<BeltBagInventoryGUI*>(mem);
        gui->_CONSTRUCTOR(inv, layout, callback);
        return gui;
    }

    void Destroy()
    {
        this->_DESTRUCTOR();
        ::operator delete(this);
    }
};

namespace
{
    float g_craftTimeMultiplier = 1.0f;
    float g_minCraftHours       = 1.0f;
    bool  g_allowBeltContainers = true;
    int   g_beltMaxStack        = 5;
    bool  g_luaRegistered       = false;
    std::vector<std::string> g_beltContainerFilter;

    __declspec(thread) bool t_searchingBeltFood = false;
    __declspec(thread) bool t_routingBeltFood   = false;

    BeltBagInventoryGUI* g_activeBeltGUI = NULL;
    ContainerItem*       g_activeBeltBag = NULL;
    InventoryGUI*        g_parentCharGUI = NULL;

    void RefreshActiveBeltGUI(Inventory* modifiedInv)
    {
        if (g_activeBeltGUI && g_activeBeltBag && modifiedInv == g_activeBeltBag->getInventory())
        {
            g_activeBeltGUI->refreshSection();
        }
    }

    void CloseActiveBeltBagGUI()
    {
        if (g_activeBeltGUI)
        {
            g_activeBeltGUI->show(false);
            g_activeBeltGUI->Destroy();
            g_activeBeltGUI = NULL;
        }
        g_activeBeltBag = NULL;
        g_parentCharGUI = NULL;
    }

    bool IsAllowedBeltContainer(Item* item)
    {
        if (!item || !g_allowBeltContainers)
            return false;

        if (item->getDataType() != CONTAINER)
            return false;

        if (g_beltContainerFilter.empty())
            return true;

        GameData* data = item->getGameData();
        if (!data)
            return false;

        for (size_t i = 0; i < g_beltContainerFilter.size(); ++i)
        {
            if (data->name == g_beltContainerFilter[i] ||
                data->stringID == g_beltContainerFilter[i])
            {
                return true;
            }
        }
        return false;
    }

    void ApplyStackBonusToBeltBag(ContainerItem* beltBag)
    {
        if (!beltBag || !beltBag->getInventory())
            return;

        lektor<InventorySection*>& sections = beltBag->getInventory()->getAllSections();
        for (unsigned int i = 0; i < sections.size(); ++i)
        {
            if (sections[i])
            {
                sections[i]->setStackingBonus(g_beltMaxStack, static_cast<float>(g_beltMaxStack));
            }
        }
    }

    ContainerItem* FindBeltContainerOnCharacter(Character* character)
    {
        if (!character)
            return NULL;

        Inventory* inv = character->getInventory();
        if (!inv)
            return NULL;

        InventorySection* beltSection = inv->getSectionOfType(ATTACH_BELT);
        if (!beltSection)
            return NULL;

        const Ogre::vector<InventorySection::SectionItem>::type& items = beltSection->getItems();
        for (size_t i = 0; i < items.size(); ++i)
        {
            Item* item = items[i].item;
            if (IsAllowedBeltContainer(item))
            {
                ContainerItem* bag = static_cast<ContainerItem*>(item);
                ApplyStackBonusToBeltBag(bag);
                return bag;
            }
        }
        return NULL;
    }

    bool IsInventoryOwnedByBeltBag(const Inventory* inv)
    {
        if (!inv)
            return false;

        if (g_activeBeltBag && g_activeBeltBag->getInventory() == inv)
            return true;

        Inventory* ncInv = const_cast<Inventory*>(inv);
        RootObject* owner = ncInv->getOwner();
        if (owner)
        {
            Item* asItem = dynamic_cast<Item*>(owner);
            if (asItem && IsAllowedBeltContainer(asItem) && asItem->slotType == ATTACH_BELT)
                return true;
        }
        return false;
    }

    void ToggleBeltBagWindow(InventoryGUI* parentGUI, ContainerItem* beltBag)
    {
        if (!parentGUI || !beltBag || !beltBag->getInventory())
            return;

        if (g_activeBeltGUI && g_activeBeltBag == beltBag)
        {
            CloseActiveBeltBagGUI();
            return;
        }

        CloseActiveBeltBagGUI();
        ApplyStackBonusToBeltBag(beltBag);

        Character* ch = parentGUI->getCallbackCharacter();
        RootObject* ownerObj = ch ? static_cast<RootObject*>(ch) : static_cast<RootObject*>(beltBag);

        InventoryLayout* layout = beltBag->createInventoryLayout();
        if (!layout)
            return;

        BeltBagInventoryGUI* bagGUI = BeltBagInventoryGUI::Create(
            beltBag->getInventory(),
            layout,
            ownerObj);

        if (!bagGUI)
            return;

        bagGUI->ownerInventory = parentGUI;

        MyGUI::types::TCoord<int> parentCoord = parentGUI->getWindowCoord();
        MyGUI::types::TCoord<int> bagCoord = bagGUI->getWindowCoord();
        int x = parentCoord.left - bagCoord.width - 8;
        if (x < 10)
            x = parentCoord.left + parentCoord.width + 8;
        int y = parentCoord.top;

        bagGUI->setPositionReal(
            static_cast<float>(x) / 1920.0f,
            static_cast<float>(y) / 1080.0f);
        bagGUI->show(true);
        bagGUI->refreshSection();

        g_activeBeltGUI = bagGUI;
        g_activeBeltBag = beltBag;
        g_parentCharGUI = parentGUI;
    }

    // --- FUNCIONES EXPUESTAS A LUA ---

    int Lua_SetCraftTimeMultiplier(lua_State* L)
    {
        double mult = KenshiLua::GetNumber(L, 1);
        if (mult > 0.01)
            g_craftTimeMultiplier = static_cast<float>(mult);

        KenshiLua::Push(L, static_cast<double>(g_craftTimeMultiplier));
        return 1;
    }

    int Lua_SetMinCraftHours(lua_State* L)
    {
        double hrs = KenshiLua::GetNumber(L, 1);
        if (hrs >= 0.0)
            g_minCraftHours = static_cast<float>(hrs);

        KenshiLua::Push(L, static_cast<double>(g_minCraftHours));
        return 1;
    }

    int Lua_SetAllowBeltContainers(lua_State* L)
    {
        g_allowBeltContainers = KenshiLua::GetBoolean(L, 1);
        return 0;
    }

    int Lua_SetBeltContainerMaxStack(lua_State* L)
    {
        int maxStack = static_cast<int>(KenshiLua::GetNumber(L, 1));
        if (maxStack >= 1)
            g_beltMaxStack = maxStack;

        KenshiLua::Push(L, static_cast<double>(g_beltMaxStack));
        return 1;
    }

    int Lua_AddBeltContainerFilter(lua_State* L)
    {
        std::string idOrName = KenshiLua::GetString(L, 1);
        if (!idOrName.empty())
            g_beltContainerFilter.push_back(idOrName);
        return 0;
    }

    int Lua_ClearBeltContainerFilter(lua_State* L)
    {
        g_beltContainerFilter.clear();
        return 0;
    }

    int Lua_GetCharacterBeltBag(lua_State* L)
    {
        Character* ch = KenshiLua::Test<Character>(L, 1);
        ContainerItem* bag = FindBeltContainerOnCharacter(ch);
        if (bag)
            KenshiLua::Push(L, static_cast<Item*>(bag));
        else
            KenshiLua::PushNil(L);
        return 1;
    }

    bool EnsureRegistered()
    {
        if (g_luaRegistered) return true;
        if (!KenshiLua::IsAvailable()) return false;

        std::string err;
        if (KenshiLua::RegisterFunction("SetCraftTimeMultiplier", Lua_SetCraftTimeMultiplier, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("SetMinCraftHours", Lua_SetMinCraftHours, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("SetAllowBeltContainers", Lua_SetAllowBeltContainers, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("SetBeltContainerMaxStack", Lua_SetBeltContainerMaxStack, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("AddBeltContainerFilter", Lua_AddBeltContainerFilter, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("ClearBeltContainerFilter", Lua_ClearBeltContainerFilter, &err) != KenshiLua::STATUS_OK ||
            KenshiLua::RegisterFunction("GetCharacterBeltBag", Lua_GetCharacterBeltBag, &err) != KenshiLua::STATUS_OK)
        {
            KenshiLua::LogError("[BeltBagAndCraft] Error registrando funciones Lua: " + err);
            return false;
        }

        g_luaRegistered = true;
        KenshiLua::LogInfo("[BeltBagAndCraft] Funciones Lua registradas.");
        return true;
    }

    // --- HOOKS DE INTERFAZ Y APILADO ---

    void (*RClickAutoEquippingOriginal)(InventoryGUI*) = NULL;
    void RClickAutoEquippingHook(InventoryGUI* self)
    {
        if (self)
        {
            Character* ch = self->getCallbackCharacter();
            if (ch && self->getInventory() == ch->getInventory())
            {
                Item* hoveredBeltItem = self->getSelectedItem("belt");
                if (hoveredBeltItem && IsAllowedBeltContainer(hoveredBeltItem))
                {
                    ToggleBeltBagWindow(self, static_cast<ContainerItem*>(hoveredBeltItem));
                    return;
                }
            }
        }

        if (RClickAutoEquippingOriginal)
            RClickAutoEquippingOriginal(self);
    }

    void (*InventoryShowOriginal)(InventoryGUI*, bool) = NULL;
    void InventoryShowHook(InventoryGUI* self, bool on)
    {
        if (InventoryShowOriginal)
            InventoryShowOriginal(self, on);

        if (!on && self && self == g_parentCharGUI)
        {
            CloseActiveBeltBagGUI();
        }
    }

    void (*InventoryUpdateOriginal)(InventoryGUI*) = NULL;
    void InventoryUpdateHook(InventoryGUI* self)
    {
        if (InventoryUpdateOriginal)
            InventoryUpdateOriginal(self);

        if (self && self == g_parentCharGUI && g_activeBeltGUI)
        {
            Character* ch = self->getCallbackCharacter();
            ContainerItem* currentBeltBag = FindBeltContainerOnCharacter(ch);
            if (!self->isVisible() || currentBeltBag != g_activeBeltBag)
            {
                CloseActiveBeltBagGUI();
            }
            else
            {
                g_activeBeltGUI->update();
            }
        }
    }

    bool (*IsWithinRangeToTradeOriginal)(InventoryGUI*, RootObject*, bool) = NULL;
    bool IsWithinRangeToTradeHook(InventoryGUI* self, RootObject* otherOwner, bool stealing)
    {
        if (self && g_activeBeltGUI && (self == g_activeBeltGUI || self == g_parentCharGUI))
        {
            if (otherOwner == g_activeBeltBag ||
                (g_parentCharGUI && otherOwner == g_parentCharGUI->getCallbackObject()))
            {
                return true;
            }
        }

        return IsWithinRangeToTradeOriginal
            ? IsWithinRangeToTradeOriginal(self, otherOwner, stealing)
            : true;
    }

    void (*SectionAddItemCbOriginal)(Inventory*, Item*) = NULL;
    void SectionAddItemCbHook(Inventory* self, Item* item)
    {
        if (SectionAddItemCbOriginal)
            SectionAddItemCbOriginal(self, item);
        RefreshActiveBeltGUI(self);
    }

    void (*SectionUpdateItemCbOriginal)(Inventory*, Item*, int) = NULL;
    void SectionUpdateItemCbHook(Inventory* self, Item* item, int unk)
    {
        if (SectionUpdateItemCbOriginal)
            SectionUpdateItemCbOriginal(self, item, unk);
        RefreshActiveBeltGUI(self);
    }

    void (*SectionRemoveItemCbOriginal)(Inventory*, Item*) = NULL;
    void SectionRemoveItemCbHook(Inventory* self, Item* item)
    {
        if (SectionRemoveItemCbOriginal)
            SectionRemoveItemCbOriginal(self, item);
        RefreshActiveBeltGUI(self);
    }

    int (*ApplyStackingBonusesOriginal)(const InventorySection*, int) = NULL;
    int ApplyStackingBonusesHook(const InventorySection* self, int normalStackableAmount)
    {
        int res = ApplyStackingBonusesOriginal
            ? ApplyStackingBonusesOriginal(self, normalStackableAmount)
            : normalStackableAmount;

        if (self && g_beltMaxStack > 1 && IsInventoryOwnedByBeltBag(self->getInventory()))
        {
            if (normalStackableAmount >= 1 && res < g_beltMaxStack)
                return g_beltMaxStack;
        }
        return res;
    }

    // --- HOOKS PARA RECOGER COMIDA AUTOMATICAMENTE EN LA CANGURERA ---

    bool (*HasRoomForItemOriginal)(Inventory*, GameData*) = NULL;
    bool HasRoomForItemHook(Inventory* self, GameData* itemData)
    {
        if (self && itemData && !t_routingBeltFood && Item::isFood(itemData))
        {
            Character* owner = self->getCallbackCharacter();
            if (owner && self == owner->getInventory())
            {
                ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
                if (beltBag && beltBag->getInventory() && beltBag->getInventory() != self)
                {
                    t_routingBeltFood = true;
                    bool beltHasRoom = HasRoomForItemOriginal
                        ? HasRoomForItemOriginal(beltBag->getInventory(), itemData)
                        : false;
                    t_routingBeltFood = false;

                    if (beltHasRoom)
                        return true;
                }
            }
        }

        return HasRoomForItemOriginal
            ? HasRoomForItemOriginal(self, itemData)
            : false;
    }

    bool (*TryAddItemOriginal)(Inventory*, Item*, int) = NULL;
    bool TryAddItemHook(Inventory* self, Item* item, int quantity)
    {
        if (self && item && !t_routingBeltFood && item->getGameData() && Item::isFood(item->getGameData()))
        {
            Character* owner = self->getCallbackCharacter();
            if (owner && self == owner->getInventory())
            {
                ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
                if (beltBag && beltBag->getInventory() && beltBag->getInventory() != self)
                {
                    t_routingBeltFood = true;
                    bool added = TryAddItemOriginal
                        ? TryAddItemOriginal(beltBag->getInventory(), item, quantity)
                        : false;
                    t_routingBeltFood = false;

                    if (added)
                    {
                        RefreshActiveBeltGUI(beltBag->getInventory());
                        return true;
                    }
                }
            }
        }

        return TryAddItemOriginal
            ? TryAddItemOriginal(self, item, quantity)
            : false;
    }

    bool (*AddItemOriginal)(Inventory*, Item*, int, bool, bool) = NULL;
    bool AddItemHook(Inventory* self, Item* item, int quantity, bool dropOnFail, bool destroyOnFail)
    {
        if (self && item && !t_routingBeltFood && item->getGameData() && Item::isFood(item->getGameData()))
        {
            Character* owner = self->getCallbackCharacter();
            if (owner && self == owner->getInventory())
            {
                ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
                if (beltBag && beltBag->getInventory() && beltBag->getInventory() != self)
                {
                    t_routingBeltFood = true;
                    // Intentar primero sin tirar al suelo ni destruir si no cabe en la cangurera
                    bool added = AddItemOriginal
                        ? AddItemOriginal(beltBag->getInventory(), item, quantity, false, false)
                        : false;
                    t_routingBeltFood = false;

                    if (added)
                    {
                        RefreshActiveBeltGUI(beltBag->getInventory());
                        return true;
                    }
                }
            }
        }

        return AddItemOriginal
            ? AddItemOriginal(self, item, quantity, dropOnFail, destroyOnFail)
            : false;
    }

    // --- HOOKS DE ALIMENTACION Y DESTRUCCION DEL PLATO VACIO ---

    bool (*RemoveItemAutoDestroyOriginal)(Inventory*, Item*, int) = NULL;
    bool RemoveItemAutoDestroyHook(Inventory* self, Item* it, int howmany)
    {
        if (self && it && !self->hasItem(it))
        {
            Character* owner = self->getCallbackCharacter();
            ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
            if (beltBag && beltBag->getInventory() && beltBag->getInventory()->hasItem(it))
            {
                bool removed = RemoveItemAutoDestroyOriginal
                    ? RemoveItemAutoDestroyOriginal(beltBag->getInventory(), it, howmany)
                    : beltBag->getInventory()->removeItemAutoDestroy(it, howmany);

                RefreshActiveBeltGUI(beltBag->getInventory());
                return removed;
            }
        }

        bool res = RemoveItemAutoDestroyOriginal
            ? RemoveItemAutoDestroyOriginal(self, it, howmany)
            : false;

        RefreshActiveBeltGUI(self);
        return res;
    }

    Item* (*InternalGetBestFoodItemOriginal)(const Inventory*, ItemFunction, Character*) = NULL;
    Item* InternalGetBestFoodItemHook(const Inventory* self, ItemFunction type, Character* race)
    {
        Item* food = InternalGetBestFoodItemOriginal ? InternalGetBestFoodItemOriginal(self, type, race) : NULL;
        if (food && food->chargesLeft <= 0.001f)
            food = NULL;

        if (food || t_searchingBeltFood || !self)
            return food;

        Character* owner = self->getCallbackCharacter();
        if (!owner) owner = race;
        if (!owner || self != owner->getInventory())
            return NULL;

        ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
        if (beltBag && beltBag->getInventory() && beltBag->getInventory() != self)
        {
            t_searchingBeltFood = true;
            food = InternalGetBestFoodItemOriginal
                ? InternalGetBestFoodItemOriginal(beltBag->getInventory(), type, race)
                : NULL;
            t_searchingBeltFood = false;

            if (food && food->chargesLeft <= 0.001f)
            {
                beltBag->getInventory()->removeItemAutoDestroy(food, 1);
                RefreshActiveBeltGUI(beltBag->getInventory());
                food = NULL;
            }
        }
        return food;
    }

    int (*InternalGetNumFoodItemsOriginal)(const Inventory*, ItemFunction, Character*) = NULL;
    int InternalGetNumFoodItemsHook(const Inventory* self, ItemFunction type, Character* race)
    {
        int count = InternalGetNumFoodItemsOriginal ? InternalGetNumFoodItemsOriginal(self, type, race) : 0;
        if (t_searchingBeltFood || !self)
            return count;

        Character* owner = self->getCallbackCharacter();
        if (!owner) owner = race;
        if (!owner || self != owner->getInventory())
            return count;

        ContainerItem* beltBag = FindBeltContainerOnCharacter(owner);
        if (beltBag && beltBag->getInventory() && beltBag->getInventory() != self)
        {
            t_searchingBeltFood = true;
            count += InternalGetNumFoodItemsOriginal
                ? InternalGetNumFoodItemsOriginal(beltBag->getInventory(), type, race)
                : 0;
            t_searchingBeltFood = false;
        }
        return count;
    }

    // --- HOOKS DE TIEMPO DE CRAFTEO ---

    CraftingItem* (*AddCraftOriginal)(CraftingBuilding*, GameData*, GameData*, float, YesNoMaybe) = NULL;
    CraftingItem* AddCraftHook(CraftingBuilding* self, GameData* basedata, GameData* matdata, float progress, YesNoMaybe crit)
    {
        CraftingItem* created = AddCraftOriginal
            ? AddCraftOriginal(self, basedata, matdata, progress, crit)
            : NULL;

        if (created)
        {
            float baseHours = (created->endTime > 0.01f) ? created->endTime : g_minCraftHours;
            created->endTime = baseHours * g_craftTimeMultiplier;
        }
        return created;
    }

    void (*CraftingOperateOriginal)(CraftingBuilding*, Character*, float) = NULL;
    void CraftingOperateHook(CraftingBuilding* self, Character* worker, float amount)
    {
        if (g_craftTimeMultiplier > 0.01f && g_craftTimeMultiplier != 1.0f)
        {
            amount /= g_craftTimeMultiplier;
        }

        if (CraftingOperateOriginal)
            CraftingOperateOriginal(self, worker, amount);
    }

    void (*CharSelectOriginal)(Character*) = NULL;
    void CharSelectHook(Character* ch)
    {
        if (CharSelectOriginal)
            CharSelectOriginal(ch);
        EnsureRegistered();
    }
}

__declspec(dllexport) void startPlugin()
{
    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&InventoryGUI::rightClickAutoEquipping),
        &RClickAutoEquippingHook,
        &RClickAutoEquippingOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&InventoryGUI::_NV_show),
        &InventoryShowHook,
        &InventoryShowOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&InventoryGUI::_NV_update),
        &InventoryUpdateHook,
        &InventoryUpdateOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&InventoryGUI::isWithinRangeToTrade),
        &IsWithinRangeToTradeHook,
        &IsWithinRangeToTradeOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV__sectionAddItemCallback),
        &SectionAddItemCbHook,
        &SectionAddItemCbOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV__sectionUpdateItemCallback),
        &SectionUpdateItemCbHook,
        &SectionUpdateItemCbOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV__sectionRemoveItemCallback),
        &SectionRemoveItemCbHook,
        &SectionRemoveItemCbOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&InventorySection::applyStackingBonuses),
        &ApplyStackingBonusesHook,
        &ApplyStackingBonusesOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV_hasRoomForItem),
        &HasRoomForItemHook,
        &HasRoomForItemOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV_tryAddItem),
        &TryAddItemHook,
        &TryAddItemOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV_addItem),
        &AddItemHook,
        &AddItemOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_NV_removeItemAutoDestroy),
        &RemoveItemAutoDestroyHook,
        &RemoveItemAutoDestroyOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_getBestFoodItem),
        &InternalGetBestFoodItemHook,
        &InternalGetBestFoodItemOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::_getNumFoodItems),
        &InternalGetNumFoodItemsHook,
        &InternalGetNumFoodItemsOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&CraftingBuilding::_addCraft),
        &AddCraftHook,
        &AddCraftOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&CraftingBuilding::_NV_operate),
        &CraftingOperateHook,
        &CraftingOperateOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Character::_NV_select),
        &CharSelectHook,
        &CharSelectOriginal);

    EnsureRegistered();
}