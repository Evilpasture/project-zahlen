// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Compiled to SPIR-V so the host can reflect every GPU ABI struct with
// SPIRV-Reflect. src/render never sees Slang source — only this bytecode.
//
// One ConstantBuffer of nested structs: a UBO cannot drop unused members, so
// every field keeps its Offset decoration and the element type names survive.
module gpu_abi;

import cluster_math;
import descriptor_heap_layout;
import instance_data;
import particles;
import push_layouts;
import uniforms;
import vertex_format;

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
    public ObjectConstants object;
    public VolumetricFogPushConstants fog;
    public VolumetricLightInjectPushConstants lightInject;
    public VolumetricTemporalPushConstants temporal;
    public DecalPushConstants decal;
    public CullingConstants culling;
    public UIObjectConstants ui;
    public ParticleComputePush particlePush;
    public MeshParticleComputePush meshParticlePush;
    public PackedVertexAttributes packedVert;
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
    acc += abi.object.instanceId;
    acc += abi.fog.volumeCount;
    acc += abi.lightInject.enableShadows;
    acc += abi.temporal.resetHistory;
    acc += abi.decal.albedoIndex;
    acc += abi.culling.drawCount;
    acc += abi.ui.albedoIdx;
    acc += abi.particlePush.particleCount;
    acc += abi.meshParticlePush.particleCount;
    acc += abi.packedVert.packedNormal;
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
