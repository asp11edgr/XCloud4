--[[
    turbo.lua - Disparo continuo (Turbo) para accesibilidad y juegos sin conexión
    Continuous fire (Turbo) for accessibility and offline games.
    XCloud4 - Script de serie / Stock script

    AVISO IMPORTANTE / IMPORTANT NOTICE:
    Este script está pensado exclusivamente para accesibilidad (reducir la fatiga en las manos)
    y para juegos en solitario / sin conexión. No lo uses en juegos multijugador online.
    This script is intended exclusively for accessibility (reducing hand fatigue) and
    single-player / offline games. Do not use it in competitive online games.
]]

-- Opciones de configuración / Configuration options:
local turbo_button = "a"       -- Botón al que aplicar turbo ("a", "b", "x", "y", "rb", "lb", "rt")
local turbo_rate_hz = 10       -- Frecuencia en pulsaciones por segundo (Hz) [1 - 25] / Rate in Hz

-- Variables internas de temporización / Internal timing state:
local timer = 0.0
local state = false

local ps_btn_map = {
        a = "cross",
        b = "circle",
        x = "square",
        y = "triangle",
        lb = "l1",
        rb = "r1",
        lt = "l2",
        rt = "r2",
    }

function on_input(pad, out, dt)
    -- Verificar si el botón está pulsado en el mando / Check if button is held on pad
    -- Mapeo DualShock 4 correspondiente / Corresponding DualShock 4 input:
    local ps_btn = ps_btn_map[turbo_button] or turbo_button
    local value = pad[ps_btn]
    local is_held = (turbo_button == "lt" or turbo_button == "rt")
        and (type(value) == "number" and value > 0.5)
        or (type(value) == "boolean" and value == true)

    if is_held then
        local period = 1.0 / turbo_rate_hz
        local half_period = period * 0.5

        timer = timer + dt
        if timer >= period then
            timer = timer % period
        end

        state = (timer < half_period)

        if turbo_button == "lt" or turbo_button == "rt" then
            out[turbo_button] = state and 1.0 or 0.0
        else
            out[turbo_button] = state
        end
    else
        timer = 0.0
        state = false
    end
end
