-- scripts/gameplay.lua
local mem = require("scripts.core.memoryview")
local ffi = require("ffi")

print("-----------------------------------------")
print(" Project-Zahlen: Scripting Engine Active ")
print("-----------------------------------------")

-- This runs every frame
function update(engine_ptr, dt)
    -- 1. Acquire the Positions and Velocities
    -- C++ side: Locks shadowLock, increments viewExportCount
    local positions = mem.C.ZHLN_GetPhysicsPositions(engine_ptr)
    local velocities = mem.C.ZHLN_GetPhysicsLinearVelocities(engine_ptr)

    local count = #positions
    
    -- 2. "NumPy style" loop
    for i = 0, count - 1 do
        -- Read a position (Python memoryview style)
        -- __index handles the stride (skipping the 'w' component automatically)
        local pos = positions[i]
        local vel = velocities[i]

        -- Logic: If a box falls too low, teleport it back up and zero its velocity
        if pos.y < -5.0 then
            pos.y = 20.0
            pos.x = (math.random() * 10) - 5
            pos.z = (math.random() * 10) - 5
            
            positions[i] = pos -- Teleport! (__newindex handles writing)
            
            vel.x, vel.y, vel.z = 0, 0, 0
            velocities[i] = vel
        end

        -- Logic: Apply a subtle "vortex" force to all moving objects
        if i > 0 then -- Don't move the floor (if floor is at index 0)
            vel.x = vel.x + (pos.z * 0.1)
            vel.z = vel.z - (pos.x * 0.1)
            velocities[i] = vel
        end
    end

    -- 3. Cleanup
    -- Explicitly releasing prevents the C++ Render loop from waiting for GC
    positions:release()
    velocities:release()
end