// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: include/Zahlen/GpuLayout.hpp
//
// GPU layout structs: generated, not written. Slang owns the GPU memory
// layout; tools/zshader reflects the cooked gpu_abi module into
// <GeneratedGpuTypes.hpp> (ZHLN::GeneratedGpu), and this header re-exports
// those structs under the engine names below. A Slang edit re-emits the
// header on the next build; src/render/GpuAbi.hpp holds every struct against
// the module through the emitted AllGpuTypes inventory, and each struct
// carries the module's offsets as static_asserts.
//
// This is the only public header that reaches the generated file, and
// deliberately not the umbrella <Zahlen/Types.hpp>: the shader tool's output
// belongs to the code that assembles GPU data, and physics, audio and the
// ECS plumbing include the umbrella without knowing a renderer exists.
// Include this where a generated struct is spelled -- and the including
// target must then compile after `zahlen_gpu_types` (see
// cmake/ShaderCompilation.cmake; the engine, render and gui targets carry
// that wiring). The Vulkan module includes no such header: its own push
// protocol numbers are hand-written in src/vulkan/pipeline/PushDataLayout.hpp
// and held against the module from the render side (GpuAbi.hpp).
//
// Push blocks are deliberately not here. What a pipeline pushes is the
// renderer's interface with its shaders, not something the engine publishes:
// those structs live in src/render/RenderInternal.hpp, and each is held against
// the module that reads it where it is pushed.

#pragma once

#include <GeneratedGpuTypes.hpp> // ZHLN::GeneratedGpu::*, the structs reflected from gpu_abi.slang

namespace ZHLN {

using InstanceData              = GeneratedGpu::InstanceData;
using Light                     = GeneratedGpu::Light;
using FrameUniforms             = GeneratedGpu::FrameUniforms;
using ClusterBounds             = GeneratedGpu::ClusterBounds;
using ClusterVolume             = GeneratedGpu::ClusterVolume;
using Particle                  = GeneratedGpu::Particle;
using Particle3D                = GeneratedGpu::Particle3D;
using ParticleEmitterParams     = GeneratedGpu::ParticleEmitterParams;
using MeshParticleEmitterParams = GeneratedGpu::MeshParticleEmitterParams;

} // namespace ZHLN
