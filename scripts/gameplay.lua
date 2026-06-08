local ffi = require("ffi")
local zahlen = require("scripts.core.zahlen")

-- Wrap the unified registry
game_ecs = game_ecs or zahlen.ecs.new(engine and engine.raw or nil)

-- ============================================================================
-- Main Game Loop Hook
-- ============================================================================
function update(ptr, dt)
    if not engine then
        engine = zahlen.wrap(ptr)
        world = engine:world()

        -- Re-instantiate the registry with the active engine pointer
        game_ecs = zahlen.ecs.new(engine.raw)
        zahlen.log("Unified ECS Registry Initialized successfully.")
    end

    if not player_ent then
        -- Query the ECS directly for the entity possessing the MovementComponent
        for ent, movement in game_ecs:view("MovementComponent") do
            player_ent = ent
            break
        end

        if player_ent then
            -- Bind dynamic Lua tables to our C++ entity ID
            game_ecs:add(player_ent, "combat", { hp = 100, max_hp = 100, is_poisoned = false })
            game_ecs:add(player_ent, "inventory", { coins = 0 })

            zahlen.log("Bound dynamic Lua components to C++ player handle: " .. tostring(player_ent))
        else
            zahlen.log("WARNING: No player entity with MovementComponent found yet.")
        end
    end

    player_input_system(game_ecs, engine)
    hybrid_health_and_speed_system(game_ecs, dt)
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

        if eng:is_key_down("SPACE") then
            movement.jumpRequested = true

            -- Play a sharp procedural jump sound! (660Hz E5 Beep, 0.1s duration)
            engine:beep(660.0, 0.1, 0.2)
        end
        -- Toggle Ragdoll State
        local is_r_down = eng:is_key_down("R")
        if is_r_down and not was_r_down then
            local ragdoll = registry:get(player_ent, "RagdollComponent")
            if ragdoll then
                if ragdoll.state == 0 then
                    -- Collapse the character physically
                    ragdoll.state = 2 -- (RagdollState::Limp)
                    zahlen.log("Player collapsed into a Limp Ragdoll!")

                    -- Play a low procedural sound to indicate a fall
                    engine:beep(150.0, 0.25, 0.3)
                else
                    -- Pull the character back up into the walking capsule
                    ragdoll.state = 0 -- (RagdollState::Inactive)
                    zahlen.log("Player stood back up!")
                end
            else
                zahlen.log("WARNING: Player entity does not have a RagdollComponent assigned.")
            end
        end
        was_r_down = is_r_down -- Update latch
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
            movement.speed = 8.0
            combat.is_poisoned = false
        end

        if combat.hp < combat.max_hp then
            combat.hp = math.min(combat.max_hp, combat.hp + 2.0 * dt)
        end

        test_environmental_damage_simulation(combat)
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
