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

    -- 2. Input and Intent Handling (Dynamic Component Updates)
    local combat = game_ecs:get(player_ent, "combat")
    local inv = game_ecs:get(player_ent, "inventory")

    if engine:is_key_down("W") then
        -- Modify dynamic Lua state
        inv.coins = inv.coins + 1
        if inv.coins % 100 == 0 then
            zahlen.log("Gold Count: " .. inv.coins)
        end
    end

    -- 3. Run dynamic Lua Systems
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