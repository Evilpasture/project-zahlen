-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("ffi")
local zh = require("scripts.core.zahlen")

-- ============================================================================
-- 1. LOAD THE MAIN MENU SYSTEM FIRST
-- ============================================================================
require("scripts.main_menu")

-- Configure post-processing settings up-front
zh:config({
    giMode = 2,
    aoRadius = 0.8,
    aoBias = 0.03,
    aoPower = 2.2,
    giIntensity = 1.5,
    giSamples = 16,
    useLocalProbe = 0,
    vignetteIntensity = 1.0,
    vignettePower = 1.8,
    enableSSR = 1,
    enableTAA = 1,
    taaFeedback = 0.95,
    ambientExposure = 5.0,
})

-- ============================================================================
-- 2. DEFINE GAMEWORLD INITIALIZATION
-- ============================================================================
function _G.StartGame()
    zh.log("[Gameplay] Initializing physics and spawning 3D assets...")

    -- Spawn static ground baseplate
    local baseplate = zh:spawn_entity({
        type = "box",
        size = zh.vec3(100.0, 1.0, 100.0),
        position = zh.vec3(0.0, -0.5, 0.0),
        color = { 0.5, 0.5, 0.5, 1.0 },
        static = true
    })
    zh.ecs:add(baseplate, "PBRComponent", { roughness = 0.15, metallic = 0.85 })
    zh.ecs:add(baseplate, "NameComponent", { name = "Baseplate" })

    -- Spawn some physical test crates
    local colors = {
        { 0.8, 0.2, 0.2, 1.0 },
        { 0.2, 0.8, 0.2, 1.0 },
        { 0.2, 0.2, 0.8, 1.0 }
    }
    for i = 1, 3 do
        local box = zh:spawn_entity({
            type = "box",
            size = zh.vec3(1.0, 1.0, 1.0),
            position = zh.vec3(0.0, 4.0 + (i * 2.2), -3.0),
            color = colors[(i % 3) + 1],
            static = false
        })
        zh.ecs:add(box, "NameComponent", { name = "Crate_" .. tostring(i) })
        zh.ecs:add(box, "PBRComponent", { roughness = 0.3, metallic = 0.1 })
    end

    -- Spawn directional sunlight
    local sun = zh:spawn_light({
        type = 0,
        rotation = { -0.575, 0.287, 0.0, 0.766 },
        color = { 1.0, 0.95, 0.88 },
        intensity = 180.0,
        radius = 0.5,
        range = 400.0
    })
    zh.ecs:add(sun, "SunTagComponent")

    zh.log("[Gameplay] Game world loaded successfully.")
end

-- ============================================================================
-- 3. BASELINE GAMEPLAY SYSTEMS (Active once player is spawned)
-- ============================================================================
local function player_input_system(dt)
    for player_ent, movement in zh.ecs:view("MovementComponent") do
        local yaw_rad = math.rad(zh.camera.yaw)
        local move_x, move_z = 0, 0
        local forward_x = math.cos(yaw_rad)
        local forward_z = math.sin(yaw_rad)
        local right_x = -math.sin(yaw_rad)
        local right_z = math.cos(yaw_rad)

        if zh.input:is_key_down("W") then
            move_x = move_x + forward_x; move_z = move_z + forward_z
        end
        if zh.input:is_key_down("S") then
            move_x = move_x - forward_x; move_z = move_z - forward_z
        end
        if zh.input:is_key_down("A") then
            move_x = move_x - right_x; move_z = move_z - right_z
        end
        if zh.input:is_key_down("D") then
            move_x = move_x + right_x; move_z = move_z + right_z
        end

        local len = math.sqrt(move_x * move_x + move_z * move_z)
        if len > 0.001 then
            move_x = move_x / len
            move_z = move_z / len
        end

        movement.inputX = move_x
        movement.inputZ = move_z
        movement.isSprinting = zh.input:is_key_down("LSHIFT") and (len > 0.001)

        if zh.input:is_key_down("SPACE") then
            movement.jumpRequested = true
        end
    end
end

zh.scheduler.register("PlayerInput", 10, player_input_system)
