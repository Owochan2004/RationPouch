-- belt_bag_craft.lua

local function init_mod()
    if SetAllowBeltContainers then
        SetAllowBeltContainers(true)
    end

    -- Cantidad maxima de comida apilable en una pila dentro de la cangurera
    if SetBeltContainerMaxStack then
        SetBeltContainerMaxStack(5)
    end

    if ClearBeltContainerFilter then
        ClearBeltContainerFilter()
        -- Opcional:
        -- AddBeltContainerFilter("Ration Pouch")
    end

    if SetMinCraftHours then
        SetMinCraftHours(1.0)
    end

    if SetCraftTimeMultiplier then
        SetCraftTimeMultiplier(3.0)
    end
end

init_mod()