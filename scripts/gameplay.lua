-- scripts/gameplay.lua
local ffi = require("ffi")
local zahlen = require("scripts.core.zahlen")

-- ============================================================================
-- System 1: Direct FFI Input Mutation (Zero-Allocation)
-- ============================================================================
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

    if engine:is_key_down("W") then
        move_x = move_x + forward_x
        move_z = move_z + forward_z
    end
    if engine:is_key_down("S") then
        move_x = move_x - forward_x
        move_z = move_z - forward_z
    end
    if engine:is_key_down("A") then
        move_x = move_x - right_x
        move_z = move_z - right_z
    end
    if engine:is_key_down("D") then
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
        movement.isSprinting = engine:is_key_down("LSHIFT") and is_moving

        if engine:is_key_down("SPACE") then
            movement.jumpRequested = true
            engine:beep(660.0, 0.1, 0.2)
        end

        local is_r_down = engine:is_key_down("R")
        if is_r_down and not was_r_down then
            local ragdoll = game_ecs:get(player_ent, "RagdollComponent")
            if ragdoll then
                if ragdoll.state == 0 then
                    ragdoll.state = 2
                    zahlen.log("Player collapsed into a Limp Ragdoll!")
                    engine:beep(150.0, 0.25, 0.3)
                else
                    ragdoll.state = 0
                    zahlen.log("Player stood back up!")
                end
            else
                zahlen.log("WARNING: Player entity does not have a RagdollComponent assigned.")
            end
        end
        was_r_down = is_r_down
    end
end

-- ============================================================================
-- System 2: Hybrid Native-Dynamic Query Loop
-- ============================================================================
local function hybrid_health_and_speed_system(dt)
    for ent, movement, combat in game_ecs:view("MovementComponent", "combat") do
        if combat.hp < 40 then
            movement.speed = 3.0

            if not combat.is_poisoned then
                zahlen.log(string.format("Entity %s is limping! HP: %d/100 (Speed throttled dynamically)",
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

-- ============================================================================
-- System 3: Smooth Scriptable Camera FOV Interpolation
-- ============================================================================
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

-- ============================================================================
-- System 4: Reactive Trigger Event System (Fiber-Based)
-- ============================================================================
collision_channel = collision_channel or zahlen.create_channel()

zahlen.task.dispatch(function()
    while true do
        local event = collision_channel:pop()
        zahlen.log("AI Woke up! Encountered trigger event: " .. event.name)
        if event.instigator == player_ent then
            zahlen.log("Citizen screams: 'Flee from the player!'")
        end
    end
end)

function on_trigger_entered(entity_id)
    collision_channel:push({
        name = "PlayerProximityAlert",
        instigator = entity_id
    })
end

-- ============================================================================
-- System Registration
-- ============================================================================
zahlen.scheduler.register("PlayerInput", 10, player_input_system)
zahlen.scheduler.register("CombatAndSpeed", 20, hybrid_health_and_speed_system)
zahlen.scheduler.register("CameraFOV", 30, camera_fov_system)

zahlen.log("Gameplay: Systems registered with the Core Scheduler.")
