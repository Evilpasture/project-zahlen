// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Compiled to SPIR-V so the host can reflect every GPU ABI struct with
// SPIRV-Reflect. src/render never sees Slang source — only this bytecode.
module gpu_abi;

import cluster_math;
import descriptor_heap_layout;
import instance_data;
import particles;
import push_layouts;
import uniforms;
import vertex_format;

ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<Light> lights;
StructuredBuffer<InstanceData> instances;
StructuredBuffer<GPUMeshlet> meshlets;
StructuredBuffer<Particle> particles2d;
StructuredBuffer<Particle3D> particles3d;
StructuredBuffer<ParticleEmitterParams> emitters;
StructuredBuffer<MeshParticleEmitterParams> meshEmitters;
StructuredBuffer<ClusterBounds> bounds;
StructuredBuffer<ClusterVolume> volumes;
StructuredBuffer<ObjectConstants> objects;
StructuredBuffer<VolumetricFogPushConstants> fogPush;
StructuredBuffer<VolumetricLightInjectPushConstants> lightPush;
StructuredBuffer<VolumetricTemporalPushConstants> temporalPush;
StructuredBuffer<DecalPushConstants> decals;
StructuredBuffer<CullingConstants> culling;
StructuredBuffer<UIObjectConstants> uiObjects;
StructuredBuffer<ParticleComputePush> particlePush;
StructuredBuffer<MeshParticleComputePush> meshParticlePush;
StructuredBuffer<PackedVertexAttributes> packedVerts;
ConstantBuffer<DescriptorHeapPushData> heapPush;

RWStructuredBuffer<uint> sink;

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint acc = 0;
    acc += asuint(frame.camPos.x);
    acc += lights[0].type;
    acc += instances[0].vertexCount;
    acc += meshlets[0].vertexCount;
    acc += asuint(particles2d[0].params.x);
    acc += asuint(particles3d[0].params.x);
    acc += emitters[0].textureIndex;
    acc += asuint(meshEmitters[0].scaleMin);
    acc += asuint(bounds[0].minPoint.x);
    acc += volumes[0].count;
    acc += objects[0].instanceId;
    acc += fogPush[0].volumeCount;
    acc += lightPush[0].enableShadows;
    acc += temporalPush[0].resetHistory;
    acc += decals[0].albedoIndex;
    acc += culling[0].drawCount;
    acc += uiObjects[0].albedoIdx;
    acc += particlePush[0].particleCount;
    acc += meshParticlePush[0].particleCount;
    acc += packedVerts[0].packedNormal;
    acc += asuint(heapPush.passData.aoRadius);
    acc += uint(heapPush.frameAddress);
    acc += uint(heapPush.lightsAddress);
    acc += uint(heapPush.instancesAddress);
    acc += uint(heapPush.jointsAddress);
    acc += uint(heapPush.previousJointsAddress);
    acc += uint(heapPush.morphDeltasAddress);
    acc += heapPush.heapIndex;
    sink[0] = acc;
}
