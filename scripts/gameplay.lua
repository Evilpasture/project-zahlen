local mem = require("scripts.core.memoryview")

-- KeyCodes from Input.hpp
local KEY = { W = 1, A = 2, S = 3, D = 4, SHIFT = 5 }

function update(engine, dt)
    -- 1. Get Physics Data
    local physBuffer = mem.C.ZHLN_GetECSBuffer(engine, "PhysicsComponent")
    local entities = mem.C.ZHLN_GetECSEntities(engine, "PhysicsComponent")
    
    -- The player is the last entity spawned in our scene setup
    local playerCount = #entities
    local playerEntityIdx = playerCount - 1 
    local playerPhysHandle = physBuffer[playerEntityIdx]

    -- 2. Player Controller Logic (Native Lua!)
    local speed = 5.0
    if mem.C.ZHLN_IsKeyDown(engine, KEY.SHIFT) == 1 then speed = 12.0 end

    local vx, vz = 0, 0
    if mem.C.ZHLN_IsKeyDown(engine, KEY.W) == 1 then vz = vz + 1 end
    if mem.C.ZHLN_IsKeyDown(engine, KEY.S) == 1 then vz = vz - 1 end
    if mem.C.ZHLN_IsKeyDown(engine, KEY.A) == 1 then vx = vx - 1 end
    if mem.C.ZHLN_IsKeyDown(engine, KEY.D) == 1 then vx = vx + 1 end

    -- Very basic camera-relative movement (assuming yaw=-90 is forward)
    -- You can pass cam.yaw into this function later for better math
    local finalVx = vx * speed
    local finalVz = vz * speed

    -- Vertical velocity (Gravity)
    -- Note: We are just overriding the horizontal, Jolt handles vertical if we let it,
    -- but CharacterVirtual usually needs manual gravity if we use SetVelocity.
    local curOnGround = mem.C.ZHLN_IsCharacterOnGround(engine, playerPhysHandle)
    local vy = curOnGround == 1 and 0 or -9.81 * dt

    mem.C.ZHLN_SetCharacterVelocity(engine, playerPhysHandle, finalVx, vy, finalVz)

    -- 3. Prop Logic (The spinning boxes)
    -- Loop through all entities WITH physics, excluding the floor (0) and player (last)
    local positions = mem.C.ZHLN_GetPhysicsPositions(engine)
    local velocities = mem.C.ZHLN_GetPhysicsLinearVelocities(engine)

    for i = 1, playerCount - 2 do
        local pos = positions[i]
        local vel = velocities[i]

        -- Pull boxes to center
        vel.x = vel.x - pos.x * dt
        vel.z = vel.z - pos.z * dt
        
        velocities[i] = vel
    end

    -- Cleanup views
    entities:release()
    physBuffer:release()
    positions:release()
    velocities:release()
end