local zahlen = require("scripts.core.zahlen")
local playerYVel = 0.0

function update(ptr, dt)
    local engine = zahlen.wrap(ptr)
    local world = engine:world()

    local entities = world:get_entities()
    local phys_buffer = world:get_component_buffer()
    local playerCount = #entities
    
    -- 1. Player (Still uses Velocity because it's a Character)
    local p_handle = phys_buffer[playerCount - 1]
    local yaw = math.rad(engine:get_camera_yaw())
    local fwd = { x = math.cos(yaw), z = math.sin(yaw) }
    local right = { x = -math.sin(yaw), z = math.cos(yaw) }

    local mx, mz = 0, 0
    if engine:is_key_down("W") then mx, mz = mx + fwd.x, mz + fwd.z end
    if engine:is_key_down("S") then mx, mz = mx - fwd.x, mz - fwd.z end
    if engine:is_key_down("A") then mx, mz = mx - right.x, mz - right.z end
    if engine:is_key_down("D") then mx, mz = mx + right.x, mz + right.z end

    local speed = engine:is_key_down("SHIFT") and 12.0 or 5.0

    if world:is_on_ground(p_handle) then
        playerYVel = -1.0 -- Snap
    else
        playerYVel = playerYVel - (30.0 * dt)
    end
    world:set_character_velocity(p_handle, mx * speed, playerYVel, mz * speed)

    -- 2. Boxes (Now using IMPULSES)
    local pos_view = world.positions
    
    -- Skip index 1 (Floor) and Last (Player)
    for i = 1, playerCount - 2 do
        local p = pos_view[i]
        local h = phys_buffer[i]

        -- Calculate a nudge toward the center
        -- We don't need to read 'vel' or calculate gravity anymore!
        -- Jolt handles all of that.
        local strength = 0.5 -- Much lower value for impulses
        local nudgeX = -p.x * strength
        local nudgeZ = -p.z * strength

        -- Apply the nudge. This feels like a "wind" or "magnet"
        -- but allows them to bounce and fall naturally.
        world:apply_impulse(h, nudgeX, 0, nudgeZ)
    end
    -- 3. Collision Events
    local contacts = world.contacts
    
    -- Using #contacts asks the C++ BufferView exactly how many events occurred this frame
    for i = 0, #contacts - 1 do
        local evt = contacts[i]

        -- Filter for heavy impacts (e.g. falling boxes hitting the floor)
        if evt.impulse > 10.0 then
            zahlen.log(
                "HEAVY IMPACT: Impulse " .. tostring(evt.impulse) ..
                " | Pos: (" .. tostring(evt.px) .. ", " .. tostring(evt.py) .. ")"
            )
        end
    end
    
    if engine:is_key_down("RBUTTON") then
        local p = pos_view[playerCount - 1] -- Player pos
        
        -- We calculate the forward direction from the camera yaw
        local hit = world:raycast(
            p.x, p.y + 0.5, p.z, -- Origin (Player chest height)
            fwd.x, 0, fwd.z,     -- Direction
            50.0,                -- Max Distance
            p_handle             -- Ignore player body!
        )
        
        if hit then
            zahlen.log("Pew! Hit entity " .. tostring(hit.entity) .. " at fraction " .. tostring(hit.fraction))
            
            -- Push the object we hit!
            world:apply_impulse(hit.entity, fwd.x * 1000, 100, fwd.z * 1000)
        end
    end
end