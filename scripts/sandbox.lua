-- Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- scripts/sandbox.lua
--
-- Stateless per-frame reducer for the default sandbox. The chunk is compiled
-- once; this outer body runs fresh for every call, so no Lua-side state can
-- accumulate across frames. C++ samples input into `in` before the call and
-- consumes the returned intents in `out` afterwards.

local TAU = math.pi * 2.0

return function(state)
    local t = state["in"].elapsed * 0.9
    return {
        lampPos = {
            x = math.cos(t) * 3.0,
            y = 4.0,
            z = math.sin(t) * 3.0,
        },
        launchSpeed = 12.0 + math.sin(state["in"].elapsed * 0.25) * 2.0,
        out = {
            spawnCube   = state["in"].keyF,
            resetCamera = state["in"].keyR,
            quit        = state["in"].keyEsc,
        },
    }
end
