# Kenshi - Belt Bag & Crafting Plugin

A native C++ RE/plugin extension for **Kenshi** that enables fully functional belt pouch/container items with native AI food routing, auto-stacking, real-time UI synchronization, and Lua-configurable crafting modifiers.

---

## 🌟 Key Features

### 🎒 Belt Container System
* **Interactive Belt Inventories:** Right-click belt pouch items while equipped to open their dedicated $3\times3$ inventory window alongside your main inventory.
* **Automatic Food Stacking:** Stack food items up to **5 units** (configurable) inside belt containers.
* **Smart AI Eating:** Characters automatically eat directly from their equipped belt bag when hungry without generating "0 nu" phantom items or endless eat loops.
* **Automated Food Gathering:** When assigned to loot or take food from storage containers, characters automatically route food into their belt bag if their main inventory is full.
* **Real-time GUI Refresh:** Instantly updates inventory grids when items are added, removed, or consumed.

### 🔨 Crafting System Modifiers
* **Crafting Time Scale:** Dynamically adjust crafting duration across benches via Lua integration.
* **Minimum Crafting Time:** Set baseline thresholds for minimum craft hours.

---

## 📜 Credits & Acknowledgments

* **3D Models & Original Concept:** Credits to **OMOCHI** for the 3D models and original design from the Steam Workshop mod **[[Abandoned] Patchwork Armour](https://steamcommunity.com/sharedfiles/filedetails/?id=1734260021)**.
* Built using native hook injections for Kenshi's engine architecture.

---

## ⚙️ Configuration (Lua API)

You can customize the plugin parameters using the provided `belt_bag_craft.lua` script.

### Available Lua Functions

| Function | Parameters | Description |
| :--- | :--- | :--- |
| `SetAllowBeltContainers(enabled)` | `boolean` | Enable or disable the belt bag inventory system. |
| `SetBeltContainerMaxStack(maxStack)` | `number` | Sets the maximum food stack size in the belt container (default: `5`). |
| `AddBeltContainerFilter(nameOrID)` | `string` | Restricts belt container behavior to specific item StringIDs or Names (Leave empty to allow all). |
| `ClearBeltContainerFilter()` | None | Clears all registered belt container filters. |
| `SetCraftTimeMultiplier(multiplier)` | `number` | Multiplies base crafting durations (e.g., `3.0` for 3x longer crafting). |
| `SetMinCraftHours(hours)` | `number` | Sets the minimum crafting time base threshold in hours. |
| `GetCharacterBeltBag(character)` | `Character*` | Returns the equipped belt container item object. |

### Example `belt_bag_craft.lua`

```lua
-- Initial setup script
local function init_mod()
    -- Enable belt containers
    if SetAllowBeltContainers then
        SetAllowBeltContainers(true)
    end

    -- Set food stacking size in belt pouches
    if SetBeltContainerMaxStack then
        SetBeltContainerMaxStack(5)
    end

    -- Optional: Restrict to specific belt pouch item IDs
    if ClearBeltContainerFilter then
        ClearBeltContainerFilter()
        -- AddBeltContainerFilter("Ration Pouch")
    end

    -- Configure craft times
    if SetMinCraftHours then
        SetMinCraftHours(1.0)
    end

    if SetCraftTimeMultiplier then
        SetCraftTimeMultiplier(3.0)
    end
end

init_mod()
```

---

## 🛠️ Installation & Requirements

1. **Requirements:**
   * Kenshi (Latest 64-bit version).
   * Kenshi Mod Loader / Injector (supporting dynamic C++ plugins & Lua bindings).

2. **Installation:**
   1. Place `BeltBagAndCraft.dll` into your injector plugin directory.
   2. Place `belt_bag_craft.lua` into your mod's script folder.
   3. Launch Kenshi via the injector/loader.

---

## 📄 License

This project is open-source. If you use or modify the codebase, please maintain credit to **OMOCHI** for the original asset models.