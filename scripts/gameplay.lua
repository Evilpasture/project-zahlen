local zahlen = require("scripts.core.zahlen")
playerYVel = 0.0
jumpForce = 15.0

function update(ptr, dt)
    engine = engine or zahlen.wrap(ptr)
    world = world or engine:world()
    
    -- Cache player handle
    player = player or world:get_entities()[#world:get_entities() - 1]

    -- 1. Intent: Calculate Direction
    local yaw = math.rad(engine:get_camera_yaw())
    local fwd = { x = math.cos(yaw), z = math.sin(yaw) }
    local right = { x = -math.sin(yaw), z = math.cos(yaw) }

    local x, z = 0, 0
    if engine:is_key_down("W") then x, z = x + fwd.x, z + fwd.z end
    if engine:is_key_down("S") then x, z = x - fwd.x, z - fwd.z end
    if engine:is_key_down("A") then x, z = x - right.x, z - right.z end
    if engine:is_key_down("D") then x, z = x + right.x, z + right.z end

    -- 2. VERBS: Just tell C++ what to do
    world:player_move(player, x, z)
    
    if engine:is_key_down("SPACE") then
        world:player_jump(player)
    end
end