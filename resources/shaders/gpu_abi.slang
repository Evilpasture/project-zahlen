// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_abi;

import cluster_math;
import descriptor_heap_layout;
import instance_data;
import particles;
import uniforms;
import gpu_push_constants;

public struct GpuAbiTypes {
    public FrameUniforms frame;
    public Light light;
    public ShadowTile shadowTile;
    public InstanceData instance;
    public GPUMeshlet meshlet;
    public Particle particle;
    public Particle3D particle3d;
    public ParticleEmitterParams emitter;
    public MeshParticleEmitterParams meshEmitter;
    public ClusterBounds bounds;
    public ClusterVolume volume;
    public DescriptorHeapPushData heap;
    public ObjectConstants objectConstants;
    public UIObjectConstants uiObjectConstants;
    public CullingConstants cullingConstants;
    public MeshParticleRenderPushConstants meshParticleRenderPush;
    public SkinningConstants skinningConstants;
    public BakePush bakePush;
    public RTShadowPushConstants rtShadowPush;
    public GtaoPushConstants gtaoPush;
    public RtgiCompositePushConstants rtgiCompositePush;
    public RtgiAtrousPushConstants rtgiAtrousPush;
    public KawasePushConstants kawasePush;
    public ThresholdPushConstants thresholdPush;
    public HdrAtrousPushConstants hdrAtrousPush;
    public BlitPushConstants blitPush;
};

ConstantBuffer<GpuAbiTypes> abi;
RWStructuredBuffer<uint> sink;

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint acc = 0;
    acc += asuint(abi.frame.camPos.x);
    acc += asuint(abi.frame.invProj[0][0]);
    acc += abi.frame.frameIndex;
    acc += asuint(abi.frame.ddgiOriginSpacing.x);
    acc += asuint(abi.frame.ddgiGridCounts.x);
    acc += asuint(abi.frame.ddgiParams.x);
    acc += uint(abi.light.type);
    acc += abi.light.shadowTileBase;
    acc += asuint(abi.shadowTile.uvOrigin.x);
    acc += asuint(abi.shadowTile.uvScale);
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
    acc += abi.objectConstants.instanceId;
    acc += abi.uiObjectConstants.albedoIdx;
    acc += uint(abi.uiObjectConstants.posAddress);
    acc += abi.cullingConstants.drawCount;
    acc += asuint(abi.cullingConstants.hizScreenSize.x);
    acc += uint(abi.meshParticleRenderPush.particleBufferAddr);
    acc += asuint(abi.meshParticleRenderPush.baseColorFactor.x);
    acc += uint(abi.skinningConstants.inPosAddr);
    acc += asuint(abi.skinningConstants.morphWeights.x);
    acc += abi.bakePush.width;
    acc += abi.rtShadowPush.samples;
    acc += abi.gtaoPush.halfRes.x;
    acc += asuint(abi.gtaoPush.rcpFullRes.x);
    acc += abi.rtgiCompositePush.fullRes.x;
    acc += abi.rtgiAtrousPush.stepSize;
    acc += asuint(abi.rtgiAtrousPush._pad.x);
    acc += uint(abi.kawasePush.mode);
    acc += asuint(abi.thresholdPush.exposure);
    acc += abi.hdrAtrousPush.stepSize;
    acc += asuint(abi.blitPush.colorFilter.x);
    sink[0] = acc;
}
