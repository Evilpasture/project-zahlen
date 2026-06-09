local ffi = require("ffi")
local zahlen = require("scripts.core.zahlen")
local InventoryShell = require("scripts.core.inventory")

-- Wrap the unified registry
game_ecs = game_ecs or zahlen.ecs.new(engine and engine.raw or nil)

-- ============================================================================
-- Main Game Loop Hook
-- ============================================================================
function update(ptr, dt)
    if not engine then
        engine = zahlen.wrap(ptr)
        world = engine:world()
        game_ecs = zahlen.ecs.new(engine.raw)
        inventory_shell = InventoryShell.new()
        zahlen.log("Unified ECS Registry Initialized successfully.")
    end

    if not player_ent then
        -- Find the player entity by looking for the movement component
        for ent, movement in game_ecs:view("MovementComponent") do
            player_ent = ent
            break
        end

        if player_ent then
            game_ecs:add(player_ent, "combat", { hp = 100, max_hp = 100, is_poisoned = false })
            game_ecs:add(player_ent, "inventory", { coins = 0, equipped = nil })
            zahlen.log("Bound dynamic Lua components to C++ player handle: " .. tostring(player_ent))

            -- EXAMPLE: Query a specific glTF sub-part by name
            local left_eye = game_ecs:find("pomni_eyes")
            if left_eye then
                zahlen.log("Successfully found Pomni's eyes in the ECS hierarchy! Handle: " .. tostring(left_eye))
            else
                zahlen.log("Could not find eyes yet (not spawned or name mismatch)")
            end
        end
    end

    player_input_system(game_ecs, engine)
    hybrid_health_and_speed_system(game_ecs, dt)
    camera_fov_system(game_ecs, engine, dt)
end

-- ============================================================================
-- System 1: Direct FFI Input Mutation (Zero-Allocation)
-- ============================================================================
local was_r_down = false
function player_input_system(registry, eng)
    if not player_ent then return end

    local move_x    = 0
    local move_z    = 0
    local yaw_rad   = math.rad(eng:get_camera_yaw())

    local forward_x = math.cos(yaw_rad)
    local forward_z = math.sin(yaw_rad)
    local right_x   = -math.sin(yaw_rad)
    local right_z   = math.cos(yaw_rad)

    if eng:is_key_down("W") then
        move_x = move_x + forward_x
        move_z = move_z + forward_z
    end
    if eng:is_key_down("S") then
        move_x = move_x - forward_x
        move_z = move_z - forward_z
    end
    if eng:is_key_down("A") then
        move_x = move_x - right_x
        move_z = move_z - right_z
    end
    if eng:is_key_down("D") then
        move_x = move_x + right_x
        move_z = move_z + right_z
    end

    local len = math.sqrt(move_x * move_x + move_z * move_z)
    if len > 0.001 then
        move_x = move_x / len
        move_z = move_z / len
    end

    -- Directly mutate memory using the centrally registered FFI type
    local movement = registry:get(player_ent, "MovementComponent")
    if movement then
        movement.inputX = move_x
        movement.inputZ = move_z

        -- Only flag sprinting if we are holding shift AND moving
        local is_moving = (move_x * move_x + move_z * move_z) > 0.001
        movement.isSprinting = eng:is_key_down("LSHIFT") and is_moving

        if eng:is_key_down("SPACE") then
            movement.jumpRequested = true
            engine:beep(660.0, 0.1, 0.2)
        end
        -- Toggle Ragdoll State
        local is_r_down = eng:is_key_down("R")
        if is_r_down and not was_r_down then
            local ragdoll = registry:get(player_ent, "RagdollComponent")
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
function hybrid_health_and_speed_system(registry, dt)
    for ent, movement, combat in registry:view("MovementComponent", "combat") do
        if combat.hp < 40 then
            movement.speed = 3.0

            if not combat.is_poisoned then
                zahlen.log(string.format("Entity %s is limping! HP: %d/100 (Speed throttled dynamically)",
                    tostring(ent), combat.hp))
                combat.is_poisoned = true
            end
        else
            -- Set speed depending on sprint state when healthy
            if movement.isSprinting then
                movement.speed = 8.0 -- Sprint Speed
            else
                movement.speed = 4.0 -- Walk Speed
            end
            combat.is_poisoned = false
        end

        if combat.hp < combat.max_hp then
            combat.hp = math.min(combat.max_hp, combat.hp + 2.0 * dt)
        end

        -- test_environmental_damage_simulation(combat)
    end
end

local damage_accumulator = 0
function test_environmental_damage_simulation(combat)
    damage_accumulator = damage_accumulator + 1
    if damage_accumulator % 500 == 0 then
        combat.hp = 25
    end
end

-- Create a global trigger volume channel
collision_channel = collision_channel or zahlen.create_channel()

-- Start a cooperative Citizen AI task
zahlen.task.dispatch(function()
    zahlen.log("Citizen AI Fiber started. Awaiting trigger event...")

    while true do
        -- 1. This SUSPENDS the Fiber.
        -- Zero CPU cycles will be wasted updating this citizen every frame.
        local event = collision_channel:pop()

        -- 2. Woke up! Someone hit our trigger volume.
        zahlen.log("AI Woke up! Encountered trigger event: " .. event.name)

        -- Process local AI reactive behavior
        if event.instigator == player_ent then
            zahlen.log("Citizen screams: 'Flee from the player!'")
        end
    end
end)

-- This function is called by the C++ Trigger Volume system when an entity steps inside
function on_trigger_entered(entity_id)
    -- Push the data to the suspended AI Fiber.
    -- It will wake up immediately on an OS worker thread this frame.
    collision_channel:push({
        name = "PlayerProximityAlert",
        instigator = entity_id
    })
end

-- ============================================================================
-- System 3: Smooth Scriptable Camera FOV Interpolation
-- ============================================================================
function camera_fov_system(registry, eng, dt)
    if not player_ent then return end

    local movement = registry:get(player_ent, "MovementComponent")
    if not movement then return end

    -- Scriptable FOV Targets
    local target_fov = 45.0 -- Base FOV
    if movement.isSprinting then
        target_fov = 55.0   -- Sprinting FOV
    end

    local current_fov = eng:get_camera_fov()

    -- Smoothly lerp towards the target FOV (factor of 8.0 provides a snappy, punchy transition)
    local new_fov = current_fov + (target_fov - current_fov) * 8.0 * dt
    eng:set_camera_fov(new_fov)
end

-- ============================================================================
-- System 4: Inventory Shell subsystem
-- ============================================================================
function run_inventory_command(cmd)
    local out = inventory_shell:execute_command(cmd)
    if out ~= "" then
        -- Route the output back to our C++ terminal log
        ffi.C.ZHLN_LogInventoryShell(out)
    end
end
