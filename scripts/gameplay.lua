-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("ffi")
local zh = require("scripts.core.zahlen")

-- ============================================================================
-- 1. CONFIGURE GRAPHICS & POST-PROCESSING ON STARTUP
-- ============================================================================
zh:config({
    giMode = 2,        -- Enable SSGI (Screen Space Global Illumination)
    aoRadius = 0.8,    -- Increase AO search radius for softer contact shadows
    aoBias = 0.03,
    aoPower = 2.2,     -- Boost AO contrast
    giIntensity = 1.5, -- Boost GI bounce intensity
    giSamples = 16,    -- Bump sample count for smoother, noise-free shadowing
    useLocalProbe = 0, -- Disable local probe so the bright outdoor sky irradiance floods the solarium
    vignetteIntensity = 1.0,
    vignettePower = 1.8,
    enableSSR = 1,
    enableTAA = 1,
    taaFeedback = 0.95,
    ambientExposure = 5.0, -- Restored to 5.0 to balance the outdoor solarium exposure
})

-- ============================================================================
-- 2. REQUIRE THE MAIN MENU SYSTEM FIRST
-- ============================================================================
-- This registers the "MainMenuUpdate" system and draws the canvas on engine.start
require("scripts.main_menu")

-- Global state gate to control gameplay system execution
_G.game_started = false

local pomni_parts = nil

-- ============================================================================
-- 3. DEFINE WORLD GENERATION / GAMESTART CALLBACK
-- ============================================================================
function _G.StartGame()
    zh.log("[Gameplay] Spawning Circus Lobby layout and characters...")

    -- 1. Initialize the deferred player, physical floor, and camera controller
    _G.player_ent = zh:dispatch("InitPlayer")

    -- 2. Spawn static world geometry and physics colliders
    zh:spawn("Circus Lobby V9.glb", { physics = true, static = true })
    pomni_parts = zh:spawn("tadc_models/POMNI.glb", { animated = true })

    -- Spawn dynamic directional sunlight (REVERTED TO CORRECT MATHEMATICAL ORIENTATION)
    local sun = zh:spawn_light({
        type = 4,                                 -- LightType::Sun
        rotation = { -0.575, 0.287, 0.0, 0.766 }, -- Aligned with the sky's baked sun disk
        color = { 1.0, 0.95, 0.88 },              -- Soft warm sunlight
        intensity = 180.0,
        radius = 0.5,
        range = 400.0
    })
    zh.ecs:add(sun, "SunTagComponent")

    -- Spawn auxiliary fill light
    zh:spawn_light({
        type = 0,
        position = { 50.0, 110.0, 34.0 },
        rotation = { 0.0, 0.0, 0.0, 1.0 },
        color = { 1.0, 0.95, 0.88 },
        intensity = 230.0,
        radius = 200.0,
        range = 400.0
    })

    -- Dynamically locate the floor mesh parts and assign metallic PBR parameters
    for ent, name_comp in zh.ecs:view("NameComponent") do
        local name_str = string.lower(ffi.string(name_comp.name))
        if string.find(name_str, "floor") or string.find(name_str, "ground") or string.find(name_str, "lobby") then
            zh.ecs:add(ent, "PBRComponent", { roughness = 0.15, metallic = 0.95 })
        end
    end

    if _G.player_ent and pomni_parts then
        local pomni_root = pomni_parts[1]
        local root_trans = zh.ecs:get(pomni_root, "TransformComponent")
        if root_trans then
            root_trans.position[0] = 0.0
            root_trans.position[1] = -0.8 -- Capsule Visual Offset
            root_trans.position[2] = 0.0
        end
        zh.ecs:add(pomni_root, "HierarchyComponent", { parent = _G.player_ent })
        zh.ecs:add(_G.player_ent, "combat", { hp = 100, max_hp = 100 })

        zh.physics:setup_ragdoll(_G.player_ent, pomni_parts)
        zh.log("[Gameplay] Skeletal Ragdoll successfully generated and bound to player controller.")
    end

    -- Release the systems to begin running
    _G.game_started = true
end

-- ============================================================================
-- 4. ECS SYSTEMS & GAMEPLAY PIPELINES (Guarded by _G.game_started)
-- ============================================================================

local was_r_down = false
local total_time = 0.0

local function player_input_system(dt)
    if not _G.game_started then return end

    for player_ent, movement in zh.ecs:view("MovementComponent") do
        local yaw_rad        = math.rad(zh.camera.yaw)

        local move_x, move_z = 0, 0
        local forward_x      = math.cos(yaw_rad)
        local forward_z      = math.sin(yaw_rad)
        local right_x        = -math.sin(yaw_rad)
        local right_z        = math.cos(yaw_rad)

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

        local is_r_down = zh.input:is_key_down("R")
        if is_r_down and not was_r_down then
            local ragdoll = zh.ecs:get(player_ent, "RagdollComponent")
            if ragdoll then
                local pomni_root = pomni_parts[1]

                if ragdoll.state == 0 then
                    ragdoll.state = 2
                    zh.log("Player collapsed into a Limp Ragdoll!")
                    zh.audio:beep(150.0, 0.25, 0.3)
                    zh.ecs:remove(pomni_root, "HierarchyComponent")
                    local root_trans = zh.ecs:get(pomni_root, "TransformComponent")
                    if root_trans then
                        root_trans.position[1] = 0.0
                    end
                else
                    ragdoll.state = 0
                    zh.log("Player stood back up!")
                    local root_trans = zh.ecs:get(pomni_root, "TransformComponent")
                    if root_trans then
                        root_trans.position[1] = -0.8
                    end
                    zh.ecs:add(pomni_root, "HierarchyComponent", { parent = player_ent })
                end
            end
        end
        was_r_down = is_r_down
    end
end

local function hybrid_health_and_speed_system(dt)
    if not _G.game_started then return end

    for ent, movement, combat in zh.ecs:view("MovementComponent", "combat") do
        if combat.hp < 40 then
            movement.speed = 3.0
            if not combat.is_poisoned then
                zh.log(string.format("Entity %s is limping! HP: %d/100 (Speed throttled dynamically)",
                    tostring(ent), combat.hp))
                combat.is_poisoned = true
            end
        else
            if movement.isSprinting then
                movement.speed = 8.0
            else
                movement.speed = 4.0
            end
            combat.is_poisoned = false
        end

        if combat.hp < combat.max_hp then
            combat.hp = math.min(combat.max_hp, combat.hp + 2.0 * dt)
        end
    end
end

local function camera_fov_system(dt)
    if not _G.game_started then return end

    for player_ent, movement in zh.ecs:view("MovementComponent") do
        for cam_ent, cam in zh.ecs:view("TargetCameraComponent") do
            if cam.target == player_ent then
                if movement.isSprinting then
                    cam.targetFov = 55.0
                else
                    cam.targetFov = 45.0
                end
            end
        end
    end
end

local function visual_feedback_system(dt)
    if not _G.game_started then return end
    total_time = total_time + dt

    for player_ent, combat in zh.ecs:view("combat") do
        for cam_ent, cam in zh.ecs:view("TargetCameraComponent") do
            if cam.target == player_ent then
                if combat.hp < 40 then
                    local pulse = math.sin(total_time * 6.0)
                    cam.vignetteIntensity = 1.4 + 0.35 * pulse
                    cam.vignettePower = 2.0
                else
                    cam.vignetteIntensity = 1.10
                    cam.vignettePower = 1.50
                end
            end
        end
    end
end

-- --- Register Systems in the Core Scheduler ---
zh.scheduler.register("PlayerInput", 10, player_input_system)
zh.scheduler.register("CombatAndSpeed", 20, hybrid_health_and_speed_system)
zh.scheduler.register("CameraFOV", 30, camera_fov_system)
zh.scheduler.register("VisualFeedback", 25, visual_feedback_system)

zh.log("[Gameplay] Systems successfully registered in dormant state.")
