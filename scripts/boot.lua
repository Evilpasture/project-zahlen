-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

-- Guard to prevent duplicating package.path on hot-reloads
if not string.find(package.path, "scripts/%%.lua") then
    package.path = package.path .. ";./scripts/?.lua;./scripts/core/?.lua"
end

-- Loads from scripts/core/fennel.lua
local fennel = require("scripts.core.fennel")
fennel.install()

require("scripts.gameplay_template")
