// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module descriptor_heap_layout;

public struct ScenePassPushConstants {
    public float4x4 invViewProj;
    public float4x4 viewProj;
    public float4 camPos;
    public int giMode;
    public float aoRadius;
    public float aoBias;
    public float aoPower;
    public float giIntensity;
    public int giSamples;
    public int enableSSR;
    public int enableRTR;
    // 1 when the dispatch composites the translucent G-buffer (the
    // "TransReflection" pass): pixels holding no translucent surface mirror the
    // opaque scene color instead of re-running the sky fill, which the opaque
    // reflection pass already composited.
    public int translucentSurface;
};

// D3D12 / DXIL binding convention (the CMake half -- ZHLN_DXIL_PROFILE and
// ZHLN_DXIL_ROOT_SIGNATURE -- lives in cmake/ShaderCompilation.cmake):
//   space0, b0  Root constants: the pass payload. ScenePassPushConstants is
//               the shared prefix; per-pass *Constants structs are the same
//               payload family. Budget 64 DWORDs, matching the host-side
//               GpuAbi::kScenePassPayloadBytes ceiling on the Vulkan push
//               blob, so a payload that fits one backend fits the other.
//   space0, b1  CBV: DescriptorHeapPushData below (frame addresses +
//               heapIndex). Vulkan pushes both halves as one push-constant
//               blob; D3D12 splits them across b0/b1 as stated here.
//   (implicit)  ResourceDescriptorHeap / SamplerDescriptorHeap: SM 6.6
//               bindless heaps take no descriptor table in the signature;
//               gpu_memory.slang indexes ResourceDescriptorHeap directly.
// [vk::push_constant] declarations in the passes stay as they are: Slang
// lowers them to the push blob on SPIR-V and to this root layout on DXIL.
public struct DescriptorHeapPushData {
    public ScenePassPushConstants passData;

    public uint64_t frameAddress;
    public uint64_t lightsAddress;
    public uint64_t instancesAddress;
    public uint64_t jointsAddress;
    public uint64_t previousJointsAddress;
    public uint64_t morphDeltasAddress;

    public uint heapIndex;
};
