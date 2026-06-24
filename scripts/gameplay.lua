-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local zh = require("scripts.core.zahlen")

-- ============================================================================
-- 1. AMBIENT & POST-PROCESSING CONFIGURATION
-- ============================================================================
zh:config({
    giMode = 2,        -- Enable SSGI (Screen Space Global Illumination)
    aoRadius = 0.8,    -- Softer contact shadows
    aoBias = 0.03,
    aoPower = 2.2,     -- Boost ambient occlusion contrast
    giIntensity = 1.5, -- Warm, bright light bounces
    giSamples = 16,    -- High sample counts to prevent specular/dither noise
    useLocalProbe = 0, -- Let the skybox irradiance flood the scene
    vignetteIntensity = 1.0,
    vignettePower = 1.8,
    enableSSR = 1, -- Enable Screen-Space Reflections
    enableTAA = 1, -- Enable Temporal Anti-Aliasing
    taaFeedback = 0.95,
    ambientExposure = 5.0,
})

-- ============================================================================
-- 2. SCENE INITIALIZATION (Baseplate, Lights & Physical Props)
-- ============================================================================
zh:on("engine.start", function()
    zh.log("[Sandbox] Initializing lightweight lighting & physics playground...")

    -- A. Spawn a Large Static Baseplate
    -- The box center is at Y = -0.5 with height = 1.0. This places the top
    -- surface exactly at Y = 0.0, aligning with the player's initial gravity drop.
    local baseplate = zh:spawn_entity({
        type = "box",
        size = zh.vec3(100.0, 1.0, 100.0),
        position = zh.vec3(0.0, -0.5, 0.0),
        color = { 0.5, 0.5, 0.5, 1.0 }, -- Neutral mid-gray to evaluate light/bounce colors
        static = true
    })

    -- Give the baseplate mirror-like reflections to test SSR/RTR
    zh.ecs:add(baseplate, "PBRComponent", { roughness = 0.15, metallic = 0.85 })
    zh.ecs:add(baseplate, "NameComponent", { name = "Baseplate" })

    -- B. Drop Dynamic Physical Props (Stacked test crates)
    zh.log("[Sandbox] Spawning dynamic physical crates...")
    local colors = {
        { 0.8, 0.2, 0.2, 1.0 }, -- Red
        { 0.2, 0.8, 0.2, 1.0 }, -- Green
        { 0.2, 0.2, 0.8, 1.0 }, -- Blue
        { 0.8, 0.8, 0.2, 1.0 }, -- Yellow
        { 0.8, 0.2, 0.8, 1.0 }  -- Magenta
    }

    for i = 1, 5 do
        -- Stacked with a slight Y-offset so they fall and collide dynamically
        local box = zh:spawn_entity({
            type = "box",
            size = zh.vec3(1.0, 1.0, 1.0),
            position = zh.vec3(0.0, 4.0 + (i * 2.2), -3.0), -- 3 meters in front of player
            color = colors[(i % 5) + 1],
            static = false
        })
        zh.ecs:add(box, "NameComponent", { name = "TestCrate_" .. tostring(i) })

        -- Vary PBR roughness & metallic properties incrementally
        zh.ecs:add(box, "PBRComponent", {
            roughness = 0.15 * i,
            metallic = 0.2 * (5 - i)
        })
    end

    -- C. Inject Lighting Workspace
    zh.log("[Sandbox] Injecting high-contrast light setup...")

    -- Direct Sun (Directional Light, type = 0)
    local sun = zh:spawn_light({
        type = 0,
        rotation = { -0.575, 0.287, 0.0, 0.766 }, -- Aligned with the sky's baked sun disk
        color = { 1.0, 0.95, 0.88 },              -- Soft warm sunlight
        intensity = 180.0,
        radius = 0.5,
        range = 400.0
    })
    zh.ecs:add(sun, "SunTagComponent")
end)

-- ============================================================================
-- 3. MOVEMENT & INTERACTION SYSTEM GRAPH
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
            zh.audio:beep(660.0, 0.1, 0.2)
        end
    end
end

zh.scheduler.register("PlayerInput", 10, player_input_system)
zh.log("[Sandbox] Test Sandbox initialization complete.")
