// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative vkCmdPushDataEXT byte layout for descriptor-heap metadata.
//
// The Vulkan mapping table consumes these values directly from push data; the
// fields are not read by shader code. The host reflects this type with Slang
// and uses the returned field offsets for both vkCmdPushDataEXT and
// VkDescriptorSetAndBindingMappingEXT. Keep the largest ordinary per-pass push
// type as the first member so descriptor-heap metadata cannot overlap it.
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
