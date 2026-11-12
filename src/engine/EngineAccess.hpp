// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Engine implementation only. This temporary seam must not be included by
// consumers, extensions, plugins, samples, tests, or generated code. Migrate
// the remaining engine users to explicit ownership and remove it.

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;
class Simulation;
class Kernel;
class World;
class RenderContext;
class PhysicsContext;
class AssetManager;
class AudioContext;
class ArticulationSystem;
class FramePlan;
class PoseUploadQueue;
struct SceneData;

namespace ECS {
class Registry;
}

struct ZHLN_API EngineAccess {
    static auto KernelOf(Engine&) noexcept -> Kernel&;
    static auto WorldOf(Engine&) noexcept -> World&;
    static auto RenderOf(Engine&) noexcept -> RenderContext&;
    static auto PhysicsOf(Engine&) noexcept -> PhysicsContext&;
    static auto AssetsOf(Engine&) noexcept -> AssetManager&;
    static auto AudioOf(Engine&) noexcept -> AudioContext&;
    static auto SceneOf(Engine&) noexcept -> SceneData&;

    // Concrete subsystem seams for engine implementation units only.
    static auto RegistryOf(Engine&) noexcept -> ECS::Registry&;
    static auto RegistryOf(const Engine&) noexcept -> const ECS::Registry&;
    static auto PoseUploadsOf(Engine&) noexcept -> PoseUploadQueue&;
    static auto ArticulationOf(Engine&) noexcept -> ArticulationSystem&;
    // Core wiring only.
    static auto FramePlanOf(Engine&) -> FramePlan&;

    // Capability -> implementation for engine code during migration.
    static auto PhysicsOf(Simulation&) noexcept -> PhysicsContext&;
};

} // namespace ZHLN
