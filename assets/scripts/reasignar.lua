--[[
    reasignar.lua - Reasignación de botones y gatillos / Button & trigger remapping
    XCloud4 - Script de serie para comodidad y accesibilidad / Stock script for comfort & accessibility

    Permite reasignar cualquier botón o gatillo del mando antes de enviarlo a Xbox.
    Allows remapping any controller button or trigger before sending it to Xbox.
]]

-- Opciones de configuración / Configuration options:
-- Modifica esta tabla para cambiar qué botón de PlayStation activa cada función de Xbox.
-- Modify this table to change which PlayStation button triggers each Xbox function.
local remap_buttons = {
    -- Ejemplo: intercambiar Cruz (A) y Círculo (B) / Example: swap Cross (A) and Circle (B)
    a = "circle",
    b = "cross",
    x = "square",
    y = "triangle",
    lb = "l1",
    rb = "r1",
    ls = "l3",
    rs = "r3",
    view = "touchpad",
    menu = "options",
    up = "up",
    down = "down",
    left = "left",
    right = "right",
}

-- Reasignación de gatillos analógicos / Analog triggers remapping:
-- "l2" -> gatillo izquierdo de PS4 / PS4 left trigger
-- "r2" -> gatillo derecho de PS4 / PS4 right trigger
local remap_triggers = {
    lt = "l2",
    rt = "r2",
}

function on_input(pad, out, dt)
    -- Aplicar remapeo de botones / Apply button remapping
    for xbox_btn, ps_btn in pairs(remap_buttons) do
        local val = pad[ps_btn]
        if val ~= nil then
            out[xbox_btn] = val
        end
    end

    -- Aplicar remapeo de gatillos / Apply trigger remapping
    for xbox_trig, ps_trig in pairs(remap_triggers) do
        local val = pad[ps_trig]
        if val ~= nil then
            out[xbox_trig] = val
        end
    end
end
