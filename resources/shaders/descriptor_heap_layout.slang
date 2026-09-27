// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module descriptor_heap_layout;

import cxx_abi;

public struct ScenePassPushConstants {
    public float4x4 invViewProj;
    public float4x4 viewProj;
    [CxxArray]
    public float4 camPos;
    public int giMode;
    public float aoRadius;
    public float aoBias;
    public float aoPower;
    public float giIntensity;
    public int giSamples;
    public int enableSSR;
    public int enableRTR;
    public int _pad;
};

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
