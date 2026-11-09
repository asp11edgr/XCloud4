--[[
    giroscopio.lua - Apuntado con giroscopio al stick derecho / Gyro aiming to right stick
    XCloud4 - Script de serie para comodidad y accesibilidad / Stock script for comfort & accessibility

    Basado en los algoritmos y conceptos de JoyShockMapper (MIT) y GyroWiki (Jibb Smart):
    - Conversión de velocidad angular (rad/s) a stick analógico
    - Calibración automática del sesgo en reposo (auto-calibration) y manual (L3 + R3)
    - Suavizado suave escalonado (smooth stepped smoothing)
    - Zona muerta de giroscopio y compensación de anti-zona muerta del juego

    Origen: JoyShockMapper (https://github.com/JibbSmart/JoyShockMapper) y GyroWiki (http://gyrowiki.jibbsmart.com)
]]

-- Opciones de configuración / Configuration options:
local aim_only = true            -- true: solo activo mientras L2 esté pulsado / only when L2 is held
local sens_x = 0.85              -- Sensibilidad horizontal (Yaw -> rx) / Horizontal sensitivity
local sens_y = 0.85              -- Sensibilidad vertical (Pitch -> ry) / Vertical sensitivity
local invert_x = false           -- Invertir eje horizontal / Invert horizontal axis
local invert_y = false           -- Invertir eje vertical / Invert vertical axis
local gyro_deadzone = 0.02       -- Zona muerta del giroscopio en rad/s / Gyro deadzone in rad/s
local game_anti_deadzone = 0.08  -- Anti-zona muerta para activar el giro en el juego / Game anti-deadzone
local smoothing_threshold = 0.35 -- Umbral rad/s bajo el cual se aplica suavizado / Smoothing threshold

-- Estado interno de calibración y suavizado / Internal calibration & smoothing state:
local bias_x = 0.0
local bias_y = 0.0
local bias_z = 0.0

local still_timer = 0.0
local still_samples = 0
local still_sum_x = 0.0
local still_sum_y = 0.0

local smoothed_x = 0.0
local smoothed_y = 0.0

local manual_cal_prev = false

local function clamp(val, min_val, max_val)
    if val < min_val then return min_val end
    if val > max_val then return max_val end
    return val
end

function on_input(pad, out, dt)
    local gx = pad.gyro.x or 0.0
    local gy = pad.gyro.y or 0.0
    local gz = pad.gyro.z or 0.0

    -- Calibración manual: pulsar L3 + R3 simultáneamente / Manual calibration: press L3 + R3 together
    local manual_cal = pad.l3 and pad.r3
    if manual_cal and not manual_cal_prev then
        bias_x = gx
        bias_y = gy
        bias_z = gz
        still_timer = 0.0
        still_samples = 0
        xcloud.notify("Giroscopio calibrado / Gyro calibrated")
    end
    manual_cal_prev = manual_cal

    -- Calibración automática en reposo / Automatic resting bias calibration
    -- Si el mando está inmóvil (velocidad angular muy baja), actualizar el sesgo promedio
    local raw_speed = math.sqrt(gx * gx + gy * gy + gz * gz)
    if raw_speed < 0.06 then
        still_timer = still_timer + dt
        still_samples = still_samples + 1
        still_sum_x = still_sum_x + gx
        still_sum_y = still_sum_y + gy
        if still_timer >= 0.75 and still_samples > 30 then
            -- Actualización lenta y suave del sesgo / Slow smooth bias tracking
            bias_x = bias_x * 0.9 + (still_sum_x / still_samples) * 0.1
            bias_y = bias_y * 0.9 + (still_sum_y / still_samples) * 0.1
            still_timer = 0.0
            still_samples = 0
            still_sum_x = 0.0
            still_sum_y = 0.0
        end
    else
        still_timer = 0.0
        still_samples = 0
        still_sum_x = 0.0
        still_sum_y = 0.0
    end

    -- Si se configuró aim_only y L2 no está presionado, omitir aplicación al stick
    local l2_val = pad.l2 or 0.0
    if aim_only and l2_val < 0.15 then
        smoothed_x = 0.0
        smoothed_y = 0.0
        return
    end

    -- Restar el sesgo de reposo calibrado / Subtract calibrated resting bias
    -- Eje X de DS4 = Pitch (arriba/abajo); Eje Y de DS4 = Yaw (giro izq/der)
    local unbias_pitch = gx - bias_x
    local unbias_yaw = gy - bias_y

    -- Aplicar zona muerta del giroscopio / Apply gyro deadzone
    local speed = math.sqrt(unbias_pitch * unbias_pitch + unbias_yaw * unbias_yaw)
    if speed <= gyro_deadzone then
        smoothed_x = 0.0
        smoothed_y = 0.0
        return
    end

    -- Suavizado escalonado suave / Smooth stepped smoothing (JoyShockMapper style)
    -- En movimientos lentos se aplica más suavizado; en giros rápidos, respuesta inmediata
    local smooth_factor = 1.0
    if speed < smoothing_threshold then
        local ratio = speed / smoothing_threshold
        smooth_factor = 0.25 + 0.75 * (ratio * ratio)
    end
    smoothed_x = smoothed_x + (unbias_yaw - smoothed_x) * smooth_factor
    smoothed_y = smoothed_y + (unbias_pitch - smoothed_y) * smooth_factor

    -- Orientación de ejes para stick derecho de Xbox (+Y es arriba, +X es derecha)
    -- Yaw gira la cámara a la derecha (positivo); Pitch inclina hacia arriba (positivo)
    local target_rx = (invert_x and -smoothed_x or smoothed_x) * sens_x
    local target_ry = (invert_y and -smoothed_y or smoothed_y) * sens_y

    -- Aplicar anti-zona muerta del juego si hay movimiento de giroscopio activo
    local gyro_mag = math.sqrt(target_rx * target_rx + target_ry * target_ry)
    if gyro_mag > 0.001 and game_anti_deadzone > 0.0 then
        local boosted_mag = game_anti_deadzone + gyro_mag * (1.0 - game_anti_deadzone)
        target_rx = (target_rx / gyro_mag) * boosted_mag
        target_ry = (target_ry / gyro_mag) * boosted_mag
    end

    -- Sumar la deflexión del giroscopio al stick derecho físico / Add gyro to physical right stick
    out.rx = clamp(out.rx + target_rx, -1.0, 1.0)
    out.ry = clamp(out.ry + target_ry, -1.0, 1.0)
end
