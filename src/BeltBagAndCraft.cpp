#define BOOST_ALL_NO_LIB
#define BOOST_NO_CXX11_ALLOCATOR
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        void* mem = Ogre::GeneralAllocatedObject::operator new(sizeof(InventoryGUI));
        BeltBagInventoryGUI* gui = static_cast<BeltBagInventoryGUI*>(mem);
        gui->_CONSTRUCTOR(inv, layout, callback);
        return gui;
    }

    void Destroy()
    {
        this->_DESTRUCTOR();
        Ogre::GeneralAllocatedObject::operator delete(this);
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

    volatile LONG g_worldTransitionDepth = 0;

    // Diagnostico de crash en %TEMP%
    const char*   g_currentHook = "none";
    const char*   g_lastHook    = "none";
    PVOID         g_vehHandle   = NULL;
    volatile LONG g_crashLogged = 0;

    struct HookScope
    {
        const char* prev;
        HookScope(const char* name) : prev(g_currentHook)
        {
            g_currentHook = name;
            g_lastHook    = name;
        }
        ~HookScope()
        {
            g_currentHook = prev;
        }
    };

    void FormatAddressModule(void* addr, char* outBuf, size_t outSize)
    {
        HMODULE hMod = NULL;
        if (addr && GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(addr),
            &hMod) && hMod)
        {
            char modPath[MAX_PATH] = { 0 };
            GetModuleFileNameA(hMod, modPath, MAX_PATH);
            const char* fileName = strrchr(modPath, '\\');
            fileName = fileName ? (fileName + 1) : modPath;
            uintptr_t offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(hMod);
            sprintf_s(outBuf, outSize, "%s + 0x%llX (0x%p)", fileName, static_cast<unsigned long long>(offset), addr);
        }
        else
        {
            sprintf_s(outBuf, outSize, "0x%p", addr);
        }
    }

    LONG CALLBACK VectoredCrashLogger(PEXCEPTION_POINTERS ep)
    {
        if (!ep || !ep->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION &&
            code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
            code != EXCEPTION_STACK_OVERFLOW &&
            code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
            code != 0xC0000409)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (InterlockedCompareExchange(&g_crashLogged, 1, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        char tempDir[MAX_PATH] = { 0 };
        char logPath[MAX_PATH] = { 0 };
        if (GetTempPathA(MAX_PATH, tempDir) > 0)
            sprintf_s(logPath, sizeof(logPath), "%sBeltBagAndCraft_crash.log", tempDir);
        else
            strcpy_s(logPath, sizeof(logPath), "BeltBagAndCraft_crash.log");

        FILE* f = NULL;
        if (fopen_s(&f, logPath, "w") == 0 && f)
        {
            char faultLoc[512] = { 0 };
            FormatAddressModule(ep->ExceptionRecord->ExceptionAddress, faultLoc, sizeof(faultLoc));

            fprintf(f, "=== BeltBagAndCraft Crash Report ===\n");
            fprintf(f, "Exception Code : 0x%08lX\n", code);
            fprintf(f, "Fault Address  : %s\n", faultLoc);
            fprintf(f, "Active Hook    : %s\n", g_currentHook ? g_currentHook : "null");
            fprintf(f, "Last Hook      : %s\n", g_lastHook ? g_lastHook : "null");

            if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            {
                fprintf(f, "AV Operation   : %s at 0x%llX\n",
                    ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" :
                    (ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC"),
                    static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
            }

            if (ep->ContextRecord)
            {
                fprintf(f, "\n--- Stack Return Addresses ---\n");
                uintptr_t* rsp = reinterpret_cast<uintptr_t*>(ep->ContextRecord->Rsp);
                int found = 0;
                for (int i = 0; i < 96 && found < 20; ++i)
                {
                    if (IsBadReadPtr(&rsp[i], sizeof(uintptr_t)))
                        break;

                    void* candidate = reinterpret_cast<void*>(rsp[i]);
                    HMODULE hMod = NULL;
                    if (candidate && GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        static_cast<LPCSTR>(candidate),
                        &hMod) && hMod)
                    {
                        char frameLoc[512] = { 0 };
                        FormatAddressModule(candidate, frameLoc, sizeof(frameLoc));
                        fprintf(f, "  [%02d] %s\n", found++, frameLoc);
                    }
                }
            }

            fflush(f);
            fclose(f);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    const int MAX_TRACKED_BELT_INVS = 128;
    const Inventory* g_knownBeltInvs[MAX_TRACKED_BELT_INVS] = { NULL };
    LONG g_knownBeltInvCursor = 0;

    __declspec(thread) bool t_searchingBeltFood = false;
    __declspec(thread) bool t_routingBeltFood   = false;

    BeltBagInventoryGUI* g_activeBeltGUI = NULL;
    ContainerItem*       g_activeBeltBag = NULL;
    InventoryGUI*        g_parentCharGUI = NULL;

    bool IsWorldTransitioning()
    {
        return InterlockedCompareExchange(&g_worldTransitionDepth, 0, 0) > 0;
    }

    void ClearTrackedBeltInventories()
    {
        memset(const_cast<Inventory**>(g_knownBeltInvs), 0, sizeof(g_knownBeltInvs));
        g_knownBeltInvCursor = 0;
    }

    void TrackBeltInventory(const Inventory* inv)
    {
        if (!inv)
            return;

        for (int i = 0; i < MAX_TRACKED_BELT_INVS; ++i)
        {
            if (g_knownBeltInvs[i] == inv)
                return;
        }

        LONG idx = InterlockedIncrement(&g_knownBeltInvCursor) - 1;
        g_knownBeltInvs[static_cast<unsigned long>(idx) % MAX_TRACKED_BELT_INVS] = inv;
    }

    bool IsTrackedBeltInventory(const Inventory* inv)
    {
        if (!inv)
            return false;

        if (g_activeBeltBag && g_activeBeltBag->getInventory() == inv)
            return true;

        for (int i = 0; i < MAX_TRACKED_BELT_INVS; ++i)
        {
            if (g_knownBeltInvs[i] == inv)
                return true;
        }
        return false;
    }

    void ResetBeltGUIStateOnly()
    {
        g_activeBeltGUI = NULL;
        g_activeBeltBag = NULL;
        g_parentCharGUI = NULL;
    }

    void RefreshActiveBeltGUI(Inventory* modifiedInv)
    {
        if (IsWorldTransitioning() || !modifiedInv)
            return;

        if (g_activeBeltGUI && g_activeBeltBag && modifiedInv == g_activeBeltBag->getInventory())
        {
            g_activeBeltGUI->refreshSection();
        }
    }

    void CloseActiveBeltBagGUI()
    {
        BeltBagInventoryGUI* guiToClose = g_activeBeltGUI;
        ResetBeltGUIStateOnly();

        if (guiToClose && !IsWorldTransitioning())
        {
            guiToClose->show(false);
            guiToClose->Destroy();
        }
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
        if (!beltBag || IsWorldTransitioning())
            return;

        Inventory* bagInv = beltBag->getInventory();
        if (!bagInv)
            return;

        TrackBeltInventory(bagInv);

        lektor<InventorySection*>& sections = bagInv->getAllSections();
        for (unsigned int i = 0; i < sections.size(); ++i)
        {
            if (sections[i])
            {
                sections[i]->setStackingBonus(g_beltMaxStack, static_cast<float>(g_beltMaxStack));
            }
        }
    }

    ContainerItem* FindBeltContainerInInventory(const Inventory* inv)
    {
        if (!inv || IsWorldTransitioning())
            return NULL;

        Inventory* ncInv = const_cast<Inventory*>(inv);
        InventorySection* beltSection = ncInv->getSectionOfType(ATTACH_BELT);
        if (!beltSection || !beltSection->getEnabled())
            return NULL;

        const Ogre::vector<InventorySection::SectionItem>::type& items = beltSection->getItems();
        for (size_t i = 0; i < items.size(); ++i)
        {
            Item* item = items[i].item;
            if (item && IsAllowedBeltContainer(item))
            {
                ContainerItem* bag = static_cast<ContainerItem*>(item);
                Inventory* bagInv = bag->getInventory();
                if (bagInv && bagInv != inv)
                {
                    TrackBeltInventory(bagInv);
                    return bag;
                }
            }
        }
        return NULL;
    }

    ContainerItem* FindBeltContainerOnCharacter(Character* character)
    {
        if (!character || IsWorldTransitioning())
            return NULL;

        return FindBeltContainerInInventory(character->getInventory());
    }

    void ToggleBeltBagWindow(InventoryGUI* parentGUI, ContainerItem* beltBag)
    {
        if (IsWorldTransitioning() || !parentGUI || !beltBag || !beltBag->getInventory())
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

    // --- PROTECCION DURANTE RESET Y CARGA DE PARTIDA ---

    void (*ClearAllOriginal)(Inventory*, bool, bool) = NULL;
    void ClearAllHook(Inventory* self, bool destroy, bool skipUnique)
    {
        HookScope scope("ClearAllHook");
        ResetBeltGUIStateOnly();

        InterlockedIncrement(&g_worldTransitionDepth);
        if (ClearAllOriginal)
            ClearAllOriginal(self, destroy, skipUnique);
        InterlockedDecrement(&g_worldTransitionDepth);
    }

    void (*LoadFromSaveStateOriginal)(Inventory*, GameSaveState*) = NULL;
    void LoadFromSaveStateHook(Inventory* self, GameSaveState* state)
    {
        HookScope scope("LoadFromSaveStateHook");
        ResetBeltGUIStateOnly();
        ClearTrackedBeltInventories();

        InterlockedIncrement(&g_worldTransitionDepth);
        if (LoadFromSaveStateOriginal)
            LoadFromSaveStateOriginal(self, state);
        InterlockedDecrement(&g_worldTransitionDepth);
    }

    void (*LoadFromContainerOriginal)(Inventory*, GameDataContainer*, GameData*) = NULL;
    void LoadFromContainerHook(Inventory* self, GameDataContainer* container, GameData* inventoryState)
    {
        HookScope scope("LoadFromContainerHook");
        ResetBeltGUIStateOnly();
        ClearTrackedBeltInventories();

        InterlockedIncrement(&g_worldTransitionDepth);
        if (LoadFromContainerOriginal)
            LoadFromContainerOriginal(self, container, inventoryState);
        InterlockedDecrement(&g_worldTransitionDepth);
    }

    // --- HOOKS DE INTERFAZ Y APILADO ---

    void (*RClickAutoEquippingOriginal)(InventoryGUI*) = NULL;
    void RClickAutoEquippingHook(InventoryGUI* self)
    {
        HookScope scope("RClickAutoEquippingHook");
        if (self && !IsWorldTransitioning())
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
        HookScope scope("InventoryShowHook");
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
        HookScope scope("InventoryUpdateHook");
        if (InventoryUpdateOriginal)
            InventoryUpdateOriginal(self);

        if (!IsWorldTransitioning() && self && self == g_parentCharGUI && g_activeBeltGUI)
        {
            Character* ch = self->getCallbackCharacter();
            ContainerItem* currentBeltBag = FindBeltContainerOnCharacter(ch);
            if (!self->isVisible() || currentBeltBag != g_activeBeltBag)
            {
                CloseActiveBeltBagGUI();
            }
            else
            {
                g_activeBeltGUI->refreshSection();
                g_activeBeltGUI->update();
            }
        }
    }

    bool (*IsWithinRangeToTradeOriginal)(InventoryGUI*, RootObject*, bool) = NULL;
    bool IsWithinRangeToTradeHook(InventoryGUI* self, RootObject* otherOwner, bool stealing)
    {
        HookScope scope("IsWithinRangeToTradeHook");
        if (!IsWorldTransitioning() && self && g_activeBeltGUI && (self == g_activeBeltGUI || self == g_parentCharGUI))
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

    int (*ApplyStackingBonusesOriginal)(const InventorySection*, int) = NULL;
    int ApplyStackingBonusesHook(const InventorySection* self, int normalStackableAmount)
    {
        HookScope scope("ApplyStackingBonusesHook");
        int res = ApplyStackingBonusesOriginal
            ? ApplyStackingBonusesOriginal(self, normalStackableAmount)
            : normalStackableAmount;

        if (!IsWorldTransitioning() && self && g_beltMaxStack > 1 && IsTrackedBeltInventory(self->getInventory()))
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
        HookScope scope("HasRoomForItemHook");
        if (!IsWorldTransitioning() && self && itemData && !t_routingBeltFood && Item::isFood(itemData))
        {
            ContainerItem* beltBag = FindBeltContainerInInventory(self);
            if (beltBag && beltBag->getInventory())
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

        return HasRoomForItemOriginal
            ? HasRoomForItemOriginal(self, itemData)
            : false;
    }

    bool (*TryAddItemOriginal)(Inventory*, Item*, int) = NULL;
    bool TryAddItemHook(Inventory* self, Item* item, int quantity)
    {
        HookScope scope("TryAddItemHook");
        if (!IsWorldTransitioning() && self && item && !t_routingBeltFood && item->getGameData() && Item::isFood(item->getGameData()))
        {
            ContainerItem* beltBag = FindBeltContainerInInventory(self);
            if (beltBag && beltBag->getInventory())
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

        return TryAddItemOriginal
            ? TryAddItemOriginal(self, item, quantity)
            : false;
    }

    // --- HOOKS DE ALIMENTACION Y DESTRUCCION DEL PLATO VACIO ---

    bool (*RemoveItemAutoDestroyOriginal)(Inventory*, Item*, int) = NULL;
    bool RemoveItemAutoDestroyHook(Inventory* self, Item* it, int howmany)
    {
        HookScope scope("RemoveItemAutoDestroyHook");
        if (!IsWorldTransitioning() && self && it && it->chargesLeft <= 0.001f &&
            it->getGameData() && Item::isFood(it->getGameData()) && !self->hasItem(it))
        {
            ContainerItem* beltBag = FindBeltContainerInInventory(self);
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
        HookScope scope("InternalGetBestFoodItemHook");
        Item* food = InternalGetBestFoodItemOriginal ? InternalGetBestFoodItemOriginal(self, type, race) : NULL;
        if (food && food->chargesLeft <= 0.001f)
            food = NULL;

        if (food || t_searchingBeltFood || IsWorldTransitioning() || !self)
            return food;

        ContainerItem* beltBag = FindBeltContainerInInventory(self);
        if (beltBag && beltBag->getInventory())
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
        HookScope scope("InternalGetNumFoodItemsHook");
        int count = InternalGetNumFoodItemsOriginal ? InternalGetNumFoodItemsOriginal(self, type, race) : 0;
        if (t_searchingBeltFood || IsWorldTransitioning() || !self)
            return count;

        ContainerItem* beltBag = FindBeltContainerInInventory(self);
        if (beltBag && beltBag->getInventory())
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
        HookScope scope("AddCraftHook");
        CraftingItem* created = AddCraftOriginal
            ? AddCraftOriginal(self, basedata, matdata, progress, crit)
            : NULL;

        if (created && !IsWorldTransitioning())
        {
            float baseHours = (created->endTime > 0.01f) ? created->endTime : g_minCraftHours;
            created->endTime = baseHours * g_craftTimeMultiplier;
        }
        return created;
    }

    void (*CraftingOperateOriginal)(CraftingBuilding*, Character*, float) = NULL;
    void CraftingOperateHook(CraftingBuilding* self, Character* worker, float amount)
    {
        HookScope scope("CraftingOperateHook");
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
        HookScope scope("CharSelectHook");
        if (CharSelectOriginal)
            CharSelectOriginal(ch);
        EnsureRegistered();
    }
}

__declspec(dllexport) void startPlugin()
{
    if (!g_vehHandle)
    {
        g_vehHandle = AddVectoredExceptionHandler(1, VectoredCrashLogger);
    }

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Inventory::clearAll),
        &ClearAllHook,
        &ClearAllOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(static_cast<void (Inventory::*)(GameSaveState*)>(&Inventory::loadFrom)),
        &LoadFromSaveStateHook,
        &LoadFromSaveStateOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(static_cast<void (Inventory::*)(GameDataContainer*, GameData*)>(&Inventory::loadFrom)),
        &LoadFromContainerHook,
        &LoadFromContainerOriginal);

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