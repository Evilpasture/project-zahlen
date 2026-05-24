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
        local entities = world:get_entities()
        player_ent = entities[#entities - 1]

        -- Bind dynamic Lua tables to our C++ entity ID
        game_ecs:add(player_ent, "combat", { hp = 100, max_hp = 100, is_poisoned = false })
        game_ecs:add(player_ent, "inventory", { coins = 0 })

        zahlen.log("Bound dynamic Lua components to C++ player handle: " .. tostring(player_ent))
    end

    player_input_system(game_ecs, engine)
    hybrid_health_and_speed_system(game_ecs, dt)
end

-- ============================================================================
-- System 1: Direct FFI Input Mutation (Zero-Allocation)
-- ============================================================================
function player_input_system(registry, eng)
    if not player_ent then return end

    local move_x = 0
    local move_z = 0
    local yaw_rad = math.rad(eng:get_camera_yaw())

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
        end
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