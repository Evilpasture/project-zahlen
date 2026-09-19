// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The host's GPU ABI types, compiled so the compiler can hold C++ to them:
// src/render/GpuAbi.hpp reads this module's own bytes and checks every leaf of
// `GPUTypes` (<Zahlen/Types.hpp>) against the declaration here, at build time.
//
// One ConstantBuffer of nested structs: a UBO cannot drop unused members, so
// every field keeps its Offset decoration and the element type names survive.
//
// What it mirrors is exactly `GPUTypes` — the buffers and uniform blocks the
// engine publishes — plus DescriptorHeapPushData, whose layout is checked the
// same way. Push blocks are not here: each is held against the module that
// reads it, where it is pushed (src/render/RenderInternal.hpp).
module gpu_abi;

import cluster_math;
import descriptor_heap_layout;
import instance_data;
import particles;
import uniforms;

public struct GpuAbiTypes {
    public FrameUniforms frame;
    public Light light;
    public InstanceData instance;
    public GPUMeshlet meshlet;
    public Particle particle;
    public Particle3D particle3d;
    public ParticleEmitterParams emitter;
    public MeshParticleEmitterParams meshEmitter;
    public ClusterBounds bounds;
    public ClusterVolume volume;
    public DescriptorHeapPushData heap;
};

ConstantBuffer<GpuAbiTypes> abi;
RWStructuredBuffer<uint> sink;

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint acc = 0;
    acc += asuint(abi.frame.camPos.x);
    acc += asuint(abi.frame.invProj[0][0]);
    acc += abi.light.type;
    acc += abi.instance.vertexCount;
    acc += abi.meshlet.vertexCount;
    acc += asuint(abi.particle.params.x);
    acc += asuint(abi.particle3d.params.x);
    acc += abi.emitter.textureIndex;
    acc += asuint(abi.meshEmitter.scaleMin);
    acc += asuint(abi.bounds.minPoint.x);
    acc += abi.volume.count;
    acc += asuint(abi.heap.passData.aoRadius);
    acc += uint(abi.heap.frameAddress);
    acc += uint(abi.heap.lightsAddress);
    acc += uint(abi.heap.instancesAddress);
    acc += uint(abi.heap.jointsAddress);
    acc += uint(abi.heap.previousJointsAddress);
    acc += uint(abi.heap.morphDeltasAddress);
    acc += abi.heap.heapIndex;
    sink[0] = acc;
}
