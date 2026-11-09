--[[
    sticks.lua - Zonas muertas, curvas de respuesta y sensibilidad / Deadzones, curves & sensitivity
    XCloud4 - Script de serie para comodidad y accesibilidad / Stock script for comfort & accessibility

    Ajusta la respuesta de los sticks analógicos (izquierdo y derecho) de forma independiente.
    Tipos de curva soportados: "lineal", "precision" (precisión mejorada), "cuadratica".
    Adjusts analog stick response independently (left and right sticks).
    Supported curve types: "linear", "precision" (enhanced precision), "quadratic".
]]

-- Opciones del stick izquierdo / Left stick options:
local left_deadzone = 0.08       -- Zona muerta central [0.0 - 0.5] / Center deadzone
local left_anti_deadzone = 0.0   -- Anti-zona muerta para juegos con zona rígida / Anti-deadzone
local left_sensitivity = 1.0     -- Multiplicador de sensibilidad [0.1 - 2.5] / Sensitivity
local left_curve = "linear"      -- "linear", "precision", o "quadratic"

-- Opciones del stick derecho / Right stick options:
local right_deadzone = 0.06      -- Zona muerta central [0.0 - 0.5] / Center deadzone
local right_anti_deadzone = 0.0  -- Anti-zona muerta / Anti-deadzone
local right_sensitivity = 1.0    -- Multiplicador de sensibilidad / Sensitivity
local right_curve = "precision"  -- "linear", "precision", o "quadratic"

local function apply_curve(val, curve_type)
    if curve_type == "precision" then
        -- Curva cúbica suave para micro-apuntado / Smooth cubic curve for micro-aiming
        return 0.5 * val + 0.5 * (val * val * val)
    elseif curve_type == "quadratic" then
        -- Curva cuadrática / Quadratic curve
        return val * val
    else
        -- Lineal por defecto / Default linear
        return val
    end
end

local function process_stick(x, y, deadzone, anti_deadzone, sens, curve_type)
    local mag = math.sqrt(x * x + y * y)
    if mag <= deadzone or mag <= 0.0001 then
        return 0.0, 0.0
    end

    -- Normalizar fuera de la zona muerta / Normalize outside deadzone
    local norm_mag = (mag - deadzone) / (1.0 - deadzone)
    if norm_mag > 1.0 then norm_mag = 1.0 end

    -- Aplicar curva seleccionada / Apply selected curve
    local curved_mag = apply_curve(norm_mag, curve_type)

    -- Aplicar anti-zona muerta y sensibilidad / Apply anti-deadzone and sensitivity
    local final_mag = anti_deadzone + curved_mag * (1.0 - anti_deadzone)
    final_mag = final_mag * sens
    if final_mag > 1.0 then final_mag = 1.0 end

    local dir_x = x / mag
    local dir_y = y / mag
    return dir_x * final_mag, dir_y * final_mag
end

function on_input(pad, out, dt)
    -- Procesar stick izquierdo / Process left stick
    local lx, ly = process_stick(pad.lx, pad.ly, left_deadzone, left_anti_deadzone, left_sensitivity, left_curve)
    out.lx = lx
    out.ly = ly

    -- Procesar stick derecho / Process right stick
    local rx, ry = process_stick(pad.rx, pad.ry, right_deadzone, right_anti_deadzone, right_sensitivity, right_curve)
    out.rx = rx
    out.ry = ry
end
