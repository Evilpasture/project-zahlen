// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BUILD-TREE ONLY. This header is deliberately NOT installed. It is the seam
// engine implementation units and in-repo tooling use to reach the concrete
// subsystems; the public Engine API exposes capabilities, not these types.
// If you are writing a consumer of the SDK, this file does not exist for you.

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
struct SceneData;

struct ZHLN_API EngineAccess {
    static auto KernelOf(Engine&) noexcept -> Kernel&;
    static auto WorldOf(Engine&) noexcept -> World&;
    static auto RenderOf(Engine&) noexcept -> RenderContext&;
    static auto PhysicsOf(Engine&) noexcept -> PhysicsContext&;
    static auto AssetsOf(Engine&) noexcept -> AssetManager&;
    static auto AudioOf(Engine&) noexcept -> AudioContext&;
    static auto SceneOf(Engine&) noexcept -> SceneData&;

    // Capability -> implementation. The only free function that still needs the
    // concrete subsystem is PushProps(Simulation, ...). This header is not
    // installed, so SDK consumers cannot reach this overload.
    static auto PhysicsOf(Simulation&) noexcept -> PhysicsContext&;
};

} // namespace ZHLN
