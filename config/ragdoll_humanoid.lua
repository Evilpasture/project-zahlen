-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- config/ragdoll_humanoid.lua
--
-- Declarative joint-matching recipe hydrated into ZHLN::RagdollAuthoring::RagdollRecipe
-- via plugins/lua/ReflectLua. Rules are evaluated in order against lower-cased
-- joint names; the first matching pattern supplies collider shape, mass, and
-- motor limits for ArticulationSystem::AttachRagdoll.

return {
    rules = {
        {
            patterns = { "hip", "pelvis", "root" },
            spec = {
                shape        = "Capsule",
                halfHeight   = 0.4,
                radius       = 0.2,
                mass         = 15.0,
                enableMotors = false,
            },
        },
        {
            patterns = { "spine", "chest", "torso" },
            spec = {
                shape         = "Capsule",
                halfHeight    = 0.5,
                radius        = 0.25,
                mass          = 20.0,
                enableMotors  = true,
                maxMotorForce = 250.0,
            },
        },
        {
            patterns = { "head", "neck" },
            spec = {
                shape         = "Sphere",
                radius        = 0.3,
                mass          = 8.0,
                enableMotors  = true,
                maxMotorForce = 250.0,
            },
        },
        {
            patterns = { "arm", "forearm", "thigh", "calf", "shin" },
            spec = {
                shape        = "Capsule",
                halfHeight   = 0.2,
                radius       = 0.1,
                mass         = 3.0,
                enableMotors = false,
            },
        },
    },
    fallback = {
        shape        = "Sphere",
        radius       = 0.08,
        mass         = 0.5,
        enableMotors = false,
    },
}
