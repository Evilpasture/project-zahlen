-- scripts/gameplay.lua
local mem = require("scripts.core.memoryview")

function update(engine_ptr, dt)
    local positions = mem.C.ZHLN_GetPhysicsPositions(engine_ptr)
    local velocities = mem.C.ZHLN_GetPhysicsLinearVelocities(engine_ptr)
    local count = #positions

    for i = 1, count - 1 do -- Start at 1 to skip the floor
        local pos = positions[i]
        local vel = velocities[i]

        -- 1. Calculate direction to center (0,0,0)
        local dirX = -pos.x
        local dirZ = -pos.z
        local dist = math.sqrt(dirX*dirX + dirZ*dirZ)

        if dist > 0.1 then
            -- 2. Apply a "Centripetal" force (Pull to center)
            vel.x = vel.x + (dirX / dist) * 2.0 * dt
            vel.z = vel.z + (dirZ / dist) * 2.0 * dt

            -- 3. Apply a "Tangential" force (Spinning around center)
            -- The vector (-z, x) is perpendicular to (x, z)
            vel.x = vel.x + (-pos.z) * 1.5 * dt
            vel.z = vel.z + (pos.x) * 1.5 * dt
        end

        velocities[i] = vel
    end

    positions:release()
    velocities:release()
end