local zahlen = require("scripts.core.zahlen")

-- Create the dynamic Lua ECS Registry
game_ecs = game_ecs or zahlen.ecs.new()

function update(ptr, dt)
    engine = engine or zahlen.wrap(ptr)
    world = world or engine:world()

    -- 1. Initialize Player and Dynamic Components
    if not player_ent then
        local entities = world:get_entities()
        player_ent = entities[#entities - 1]

        -- Dynamically attach dynamic Lua components to the C++ player handle!
        game_ecs:add(player_ent, "combat", { hp = 100, max_hp = 100 })
        game_ecs:add(player_ent, "inventory", { coins = 0 })

        zahlen.log("Attached combat & inventory components to C++ player handle!")
    end

    -- 2. Fetch Dynamic Components
    local combat    = game_ecs:get(player_ent, "combat")
    local inv       = game_ecs:get(player_ent, "inventory")

    -- 3. Calculate Camera-Relative Movement
    local move_x    = 0
    local move_z    = 0

    -- Fetch the camera yaw from C++ (in degrees) and convert to radians
    local yaw_rad   = math.rad(engine:get_camera_yaw())

    -- Direction vectors on the horizontal (XZ) plane
    local forward_x = math.cos(yaw_rad)
    local forward_z = math.sin(yaw_rad)
    local right_x   = -math.sin(yaw_rad)
    local right_z   = math.cos(yaw_rad)

    -- Accumulate directional input
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

    -- Normalize vector so diagonal movement isn't faster
    local len = math.sqrt(move_x * move_x + move_z * move_z)
    if len > 0.001 then
        move_x = move_x / len
        move_z = move_z / len
    end

    -- Send movement intent back to C++
    world:set_movement_input(player_ent, move_x, move_z)

    -- 4. Handle Jumping
    if engine:is_key_down("SPACE") then
        world:set_jump_intent(player_ent)
    end

    -- 5. Extra Gameplay Loop Hook (e.g. Increment coins while moving)
    if len > 0.001 then
        inv.coins = inv.coins + 1
        if inv.coins % 100 == 0 then
            zahlen.log("Gold Count: " .. inv.coins)
        end
    end

    -- 6. Run dynamic Lua Systems
    health_regeneration_system(game_ecs, dt)
end

-- A purely dynamic system iterating over Lua components
function health_regeneration_system(registry, dt)
    for ent, combat in registry:view("combat") do
        if combat.hp < combat.max_hp then
            combat.hp = math.min(combat.max_hp, combat.hp + 5.0 * dt)
        end
    end
end
