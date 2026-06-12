-- scripts/gameplay.lua
local zh = require("scripts.core.zahlen")

-- --- Ambient & Post-Processing Subsystems ---
zh.config({
    giMode = 1,
    aoRadius = 0.5,
    aoBias = 0.05,
    aoPower = 1.8,
    giIntensity = 1.2,
    giSamples = 8,
    useLocalProbe = 1,
    probeMin = { -22.0, 0.0, -22.0 },
    probeMax = { 22.0, 12.0, 22.0 },
    probePos = { 0.0, 4.0, 0.0 },
    vignetteIntensity = 1.10,
    vignettePower = 1.50,
    enableSSR = 1,
    floorRoughness = 0.15,
    floorMetallic = 0.95,
    sphereLightRadius = 1.5,
    light1Intensity = 180.0,
    light2Intensity = 180.0,
    enableTAA = 1,
    taaFeedback = 0.95,
})

-- --- Autostart Layout ---
zh.on("engine.start", function()
    zh.log("Scene: Spawning declarative layout...")

    zh.spawn("Circus Lobby V9.glb", { physics = true, static = true })
    pomni_parts = zh.spawn("tadc_models/POMNI.glb", { animated = true })

    -- Immediate access to findings
    local floor = zh.find("floor")
    if floor then
        zh.log("Scene: Found floor, binding immediate state.")
    end

    if player_ent and pomni_parts then
        zh.register_player_parts(pomni_parts)
        zh.dispatch(zh.dsp.ragdoll.setup(player_ent, pomni_parts))
        zh.log("Scene: Skeletal Ragdoll successfully generated and bound to player controller.")
    end

    -- Create and register debug line resources inside Lua!
    local line_mesh = zh.create_box(0.02, 0.02, 0.5, 0, 1, 1, 1)
    local pipeline, albedo = zh.create_material(0, 1, 1, 1)
    zh.register_debug_line(line_mesh, pipeline, albedo)
end)

-- ==========================================
-- SYSTEMS & GAMEPLAY PIPELINES (Event/Tick driven)
-- ==========================================

local was_r_down = false

local function player_input_system(dt)
    if not player_ent then return end

    local move_x    = 0
    local move_z    = 0
    local yaw_rad   = math.rad(engine:get_camera_yaw())

    local forward_x = math.cos(yaw_rad)
    local forward_z = math.sin(yaw_rad)
    local right_x   = -math.sin(yaw_rad)
    local right_z   = math.cos(yaw_rad)

    if zh.is_key_down("W") then
        move_x = move_x + forward_x
        move_z = move_z + forward_z
    end
    if zh.is_key_down("S") then
        move_x = move_x - forward_x
        move_z = move_z - forward_z
    end
    if zh.is_key_down("A") then
        move_x = move_x - right_x
        move_z = move_z - right_z
    end
    if zh.is_key_down("D") then
        move_x = move_x + right_x
        move_z = move_z + right_z
    end

    local len = math.sqrt(move_x * move_x + move_z * move_z)
    if len > 0.001 then
        move_x = move_x / len
        move_z = move_z / len
    end

    local movement = game_ecs:get(player_ent, "MovementComponent")
    if movement then
        movement.inputX = move_x
        movement.inputZ = move_z

        local is_moving = (move_x * move_x + move_z * move_z) > 0.001
        movement.isSprinting = zh.is_key_down("LSHIFT") and is_moving

        if zh.is_key_down("SPACE") then
            movement.jumpRequested = true
            zh.dispatch(zh.dsp.sound.beep(660.0, 0.1, 0.2))
        end

        local is_r_down = zh.is_key_down("R")
        if is_r_down and not was_r_down then
            local ragdoll = game_ecs:get(player_ent, "RagdollComponent")
            if ragdoll then
                if ragdoll.state == 0 then
                    ragdoll.state = 2
                    zh.log("Player collapsed into a Limp Ragdoll!")
                    zh.dispatch(zh.dsp.sound.beep(150.0, 0.25, 0.3))
                else
                    ragdoll.state = 0
                    zh.log("Player stood back up!")
                end
            else
                zh.log("WARNING: Player entity does not have a RagdollComponent assigned.")
            end
        end
        was_r_down = is_r_down
    end
end

local function hybrid_health_and_speed_system(dt)
    for ent, movement, combat in game_ecs:view("MovementComponent", "combat") do
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
    if not player_ent then return end

    local movement = game_ecs:get(player_ent, "MovementComponent")
    if not movement then return end

    local target_fov = 45.0
    if movement.isSprinting then
        target_fov = 55.0
    end

    local current_fov = engine:get_camera_fov()
    local new_fov = current_fov + (target_fov - current_fov) * 8.0 * dt
    engine:set_camera_fov(new_fov)
end

local function visual_feedback_system(dt)
    if not player_ent or not game_state then return end

    local combat = game_ecs:get(player_ent, "combat")
    if not combat then return end

    if combat.hp < 40 then
        local pulse = math.sin(engine:get_total_time() * 6.0)
        game_state.vignetteIntensity = 1.4 + 0.35 * pulse
        game_state.vignettePower = 2.0
    else
        game_state.vignetteIntensity = 1.10
        game_state.vignettePower = 1.50
    end
end

-- --- Register Systems in the Core Scheduler ---
zh.scheduler.register("PlayerInput", 10, player_input_system)
zh.scheduler.register("CombatAndSpeed", 20, hybrid_health_and_speed_system)
zh.scheduler.register("CameraFOV", 30, camera_fov_system)
zh.scheduler.register("VisualFeedback", 25, visual_feedback_system)

zh.log("Gameplay: Systems successfully initialized under the Core Scheduler.")

if not _G.engine_started then
    _G.engine_started = true
    zh.trigger("engine.start")
end
