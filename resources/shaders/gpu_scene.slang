// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_scene;

import common;
import gpu_buffer;
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

    GPUBuffer<InstanceData> src = GPUBuffer<InstanceData>(pc.srcInstancesAddr);
    InstanceData inst = src.load(tid.x);

    bool visible = inst.vertexCount > 0 && inst.cullRadius > 0.0f;
    if (!visible)
        return;

    GPUBuffer<uint> counter = GPUBuffer<uint>(pc.counterAddr);
    uint slot = counter.atomicAdd(0u, 1u);
    if (slot >= pc.maxDstCount)
        return;

    GPUBuffer<InstanceData> dst = GPUBuffer<InstanceData>(pc.dstInstancesAddr);
    dst.store(slot, inst);

    GPUBuffer<GPUDrawIndirectCommand> cmds = GPUBuffer<GPUDrawIndirectCommand>(pc.drawCmdsAddr);
    if (cmds.isValid()) {
        GPUDrawIndirectCommand cmd;
        cmd.vertexCount   = (inst.indexCount > 0) ? inst.indexCount : inst.vertexCount;
        cmd.instanceCount = 1;
        cmd.firstVertex   = 0;
        cmd.firstInstance = slot;
        cmds.store(slot, cmd);
    }

    GPUBuffer<GPUMeshTasksIndirectCommand> meshCmds = GPUBuffer<GPUMeshTasksIndirectCommand>(pc.meshCmdsAddr);
    if (meshCmds.isValid() && inst.meshletCount > 0) {
        GPUMeshTasksIndirectCommand meshCmd;
        meshCmd.groupCountX = (inst.meshletCount + 31u) / 32u;
        meshCmd.groupCountY = 1;
        meshCmd.groupCountZ = 1;
        meshCmds.store(slot, meshCmd);
    }
}
