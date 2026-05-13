local zhln = require("scripts.core.zhln")
local playerYVel = 0.0

function update(ptr, dt)
    local engine = zhln.wrap(ptr)
    local world = engine:world()

    local entities = world:get_entities()
    local phys_buffer = world:get_component_buffer()
    
    local p_idx = #entities - 1
    local p_handle = phys_buffer[p_idx]

    -- Camera-relative math
    local yaw = math.rad(engine:get_camera_yaw())
    local fwd = { x = math.cos(yaw), z = math.sin(yaw) }
    local right = { x = -math.sin(yaw), z = math.cos(yaw) }

    local mx, mz = 0, 0
    if engine:is_key_down("W") then mx, mz = mx + fwd.x, mz + fwd.z end
    if engine:is_key_down("S") then mx, mz = mx - fwd.x, mz - fwd.z end
    if engine:is_key_down("A") then mx, mz = mx - right.x, mz - right.z end
    if engine:is_key_down("D") then mx, mz = mx + right.x, mz + right.z end

    -- Normalization
    local len = math.sqrt(mx*mx + mz*mz)
    if len > 0.01 then mx, mz = (mx/len), (mz/len) end

    local speed = engine:is_key_down("SHIFT") and 12.0 or 5.0

    -- Gravity
    if world:is_on_ground(p_handle) then
        playerYVel = 0.0
    else
        playerYVel = playerYVel - (30.0 * dt)
    end

    world:set_character_velocity(p_handle, mx * speed, playerYVel, mz * speed)

    -- swarming boxes
    local pos = world.positions
    local vel = world.velocities
    for i = 1, #entities - 2 do
        local p, v, h = pos[i], vel[i], phys_buffer[i]
        world:set_linear_velocity(h, v.x - p.x * dt, v.y, v.z - p.z * dt)
    end
end