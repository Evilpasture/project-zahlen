-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- config/graphics.lua
--
-- Declarative, hardware-adaptive graphics profile evaluated once on startup
-- (and transactionally hot-reloaded on save) by plugins/lua/ReflectLua.
-- Receives a read-only snapshot of ZHLN::RenderInfo (`hw`) and returns a table
-- overlaid onto ZHLN::GraphicsSettings. No FFI, no I/O, no per-frame hooks.

return function(hw)
    local isDiscrete = (hw.deviceType == "DiscreteGPU")
    local hasRT      = hw.rayTracingSupported and isDiscrete

    local preset = "Medium"
    if hasRT then
        preset = "Ultra"
    elseif isDiscrete then
        preset = "High"
    elseif hw.deviceType == "CPU" then
        preset = "Low"
    end

    return {
        qualityPreset = preset,
        antiAliasing = {
            mode = (hw.deviceType == "CPU") and "FXAA" or "SMAA",
        },
        post = {
            enableSSR = (not hasRT and isDiscrete) and 1 or 0,
            enableRTR = hasRT and 1 or 0,
        },
        shadows = {
            resolution = hasRT and 4096 or (isDiscrete and 2048 or 1024),
        },
        rayTracing = {
            enableReflections = hasRT,
            enableShadows     = hasRT,
            enableDiffuseGI   = hasRT,
        },
    }
end
