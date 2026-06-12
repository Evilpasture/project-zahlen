-- scripts/core/scheduler.lua
local Scheduler = {
    systems = {}, -- Array of { name, priority, fn, enabled }
}

function Scheduler:register(name, priority, fn)
    table.insert(self.systems, {
        name = name,
        priority = priority or 100,
        fn = fn,
        enabled = true
    })
    -- Sort systems by priority (lower number runs first)
    table.sort(self.systems, function(a, b) return a.priority < b.priority end)
end

function Scheduler:set_system_enabled(name, enabled)
    for _, sys in ipairs(self.systems) do
        if sys.name == name then
            sys.enabled = enabled
            break
        end
    end
end

function Scheduler:update(registry, dt)
    for i = 1, #self.systems do
        local sys = self.systems[i]
        if sys.enabled then
            sys.fn(registry, dt)
        end
    end
end

return Scheduler
