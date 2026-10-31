// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What an emitter *is*, in scene terms.
//
// These are the parameters a host authors: gravity, drag, the spawn volume, the
// lifetime and velocity ranges, the colour ramp, sizes, spin, and how the emitted
// billboards face the camera. They are value types (ZHLN::Float2/Float3/Float4), so
// a host includes no Jolt header to author one, with no padding, no lane-sharing
// and no field the renderer owns -- the texture, the
// blend mode and the storage layout are not emitter physics.
//
// The shader reads its own struct (ParticleEmitterParams, emitted from the Slang
// module by utils/zshader), and src/render/GpuPack.cpp writes that struct from these
// fields at the submit boundary. So a host never sees an offset, a stub, or a texture
// index it had to resolve itself: Components.hpp used to hold that ABI struct by
// value, which put the GPU layout inside every system that includes a component.

#include <Zahlen/Math/ValueTypes.hpp>
#include <Zahlen/Render/GpuEnums.hpp>

namespace ZHLN {

// One particle emitter's physics. Field for field what the update and render passes
// need; the order here is the author's order, not the shader's.
struct ParticleEmitterDesc {
    ZHLN::Float3 gravity {0.0f, -9.81f, 0.0f};
    float        drag = 0.2f;

    ZHLN::Float3 turbulence {0.0f, 0.0f, 0.0f};
    float        turbulenceFreq = 0.1f;

    // The spawn volume: a sphere of spawnRadius, or the box spawnBoxExtent when the
    // radius is zero. A non-zero loopBoundary wraps particles past it.
    ZHLN::Float3 spawnOrigin {0.0f, 0.0f, 0.0f};
    float        spawnRadius = 0.0f;
    ZHLN::Float3 spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float        loopBoundary = 0.0f;

    ZHLN::Float3 initVelMin {-1.0f, -1.0f, -1.0f};
    float        lifetimeMin = 1.0f;
    ZHLN::Float3 initVelMax {1.0f, 1.0f, 1.0f};
    float        lifetimeMax = 3.0f;

    ZHLN::Float4 startColor {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::Float4 endColor {1.0f, 1.0f, 1.0f, 0.0f};
    // Width and height of the billboard a particle is born with, and the pair it
    // dies with. Two components of plain data, not a shader lane.
    ZHLN::Float2 startSize {0.1f, 0.1f};
    ZHLN::Float2 endSize {0.0f, 0.0f};

    float spinSpeed = 0.0f;
    // How the emitted quad faces the world: the camera, its own velocity, or the
    // ground plane.
    ParticleAlignment alignment = ParticleAlignment::CameraBillboard;
};

// The same, for emitters whose particles are meshes: rotation velocities and a
// uniform scale range stand in for the billboard sizes.
struct MeshParticleEmitterDesc {
    ZHLN::Float3 gravity {0.0f, -9.81f, 0.0f};
    float        drag = 0.2f;

    ZHLN::Float3 turbulence {0.0f, 0.0f, 0.0f};
    float        turbulenceFreq = 0.1f;

    ZHLN::Float3 spawnOrigin {0.0f, 0.0f, 0.0f};
    float        spawnRadius = 0.0f;
    ZHLN::Float3 spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float        loopBoundary = 0.0f;

    ZHLN::Float3 initVelMin {-5.0f, 0.0f, -5.0f};
    float        lifetimeMin = 1.0f;
    ZHLN::Float3 initVelMax {5.0f, 10.0f, 5.0f};
    float        lifetimeMax = 3.0f;

    ZHLN::Float3 rotVelMin {-3.14f, -3.14f, -3.14f};
    float        scaleMin = 0.1f;
    ZHLN::Float3 rotVelMax {3.14f, 3.14f, 3.14f};
    float        scaleMax = 0.5f;

    ZHLN::Float4 startColor {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::Float4 endColor {1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace ZHLN
