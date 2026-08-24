// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU-driven scene compaction. Reads ECS-uploaded instance descriptors and
// emits compacted InstanceData plus VkDrawIndirectCommand / mesh-task
// arguments. The host only supplies device addresses.
module gpu_scene;

import common;
import uniforms;

public struct GPUDrawIndirectCommand {
    public uint vertexCount;
    public uint instanceCount;
    public uint firstVertex;
    public uint firstInstance;
};

public struct GPUMeshTasksIndirectCommand {
    public uint groupCountX;
    public uint groupCountY;
    public uint groupCountZ;
};

public struct GPUScenePush {
    public uint64_t srcInstancesAddr;
    public uint64_t dstInstancesAddr;
    public uint64_t drawCmdsAddr;
    public uint64_t meshCmdsAddr;
    public uint64_t counterAddr;
    public uint srcCount;
    public uint maxDstCount;
};

[vk::push_constant]
GPUScenePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void CompactMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.srcCount)
        return;

    InstanceData* src = (InstanceData*) pc.srcInstancesAddr;
    InstanceData inst = src[tid.x];

    bool visible = inst.vertexCount > 0 && inst.cullRadius > 0.0f;
    if (!visible)
        return;

    uint* counter = (uint*) pc.counterAddr;
    uint slot     = 0;
    InterlockedAdd(counter[0], 1, slot);
    if (slot >= pc.maxDstCount)
        return;

    InstanceData* dst = (InstanceData*) pc.dstInstancesAddr;
    dst[slot]         = inst;

    if (pc.drawCmdsAddr != 0) {
        GPUDrawIndirectCommand* cmds = (GPUDrawIndirectCommand*) pc.drawCmdsAddr;
        GPUDrawIndirectCommand cmd;
        cmd.vertexCount   = (inst.indexCount > 0) ? inst.indexCount : inst.vertexCount;
        cmd.instanceCount = 1;
        cmd.firstVertex   = 0;
        cmd.firstInstance = slot;
        cmds[slot]        = cmd;
    }

    if (pc.meshCmdsAddr != 0 && inst.meshletCount > 0) {
        GPUMeshTasksIndirectCommand* meshCmds = (GPUMeshTasksIndirectCommand*) pc.meshCmdsAddr;
        GPUMeshTasksIndirectCommand meshCmd;
        meshCmd.groupCountX = (inst.meshletCount + 31u) / 32u;
        meshCmd.groupCountY = 1;
        meshCmd.groupCountZ = 1;
        meshCmds[slot]      = meshCmd;
    }
}
