// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_memory;

#if defined(__TARGET_DXIL__) || defined(__TARGET_HLSL__)
ByteAddressBuffer GetD3D12Buffer(uint encodedSlot)
{
    // The CPU wire format stores slot + 1 so raw zero remains the null address.
    return ResourceDescriptorHeap[NonUniformResourceIndex(encodedSlot - 1u)];
}

RWByteAddressBuffer GetD3D12RWBuffer(uint encodedSlot)
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(encodedSlot - 1u)];
}
#endif

// Backend-specific pointer lowering is deliberately isolated in this module.
// The 64-bit wire representation is Vk BDA on SPIR-V and {one-based heap slot,
// byte offset} on DXIL. Shader passes should import this module rather than
// inspect or unpack raw addresses themselves.
public struct DeviceAddress<T>
{
    public uint64_t raw;

    public __init(uint64_t raw)
    {
        this.raw = raw;
    }

    [ForceInline]
    public bool isValid()
    {
        return this.raw != 0;
    }

    [ForceInline]
    public T load(uint index = 0)
    {
#if defined(__TARGET_SPIRV__)
        return ((T*)this.raw)[index];
#else
        uint encodedSlot = (uint)(this.raw >> 32);
        uint offset = (uint)(this.raw & 0xFFFFFFFFu) + index * (uint)sizeof(T);
        return GetD3D12Buffer(encodedSlot).Load<T>(offset);
#endif
    }

    [ForceInline]
    public void store(uint index, T value)
    {
#if defined(__TARGET_SPIRV__)
        ((T*)this.raw)[index] = value;
#else
        uint encodedSlot = (uint)(this.raw >> 32);
        uint offset = (uint)(this.raw & 0xFFFFFFFFu) + index * (uint)sizeof(T);
        GetD3D12RWBuffer(encodedSlot).Store<T>(offset, value);
#endif
    }
}

// Slang operators are module-level overloads (unlike C++ member operators).
[ForceInline]
public DeviceAddress<T> operator+<T>(DeviceAddress<T> address, uint count)
{
    if (address.raw == 0)
        return address;
#if defined(__TARGET_SPIRV__)
    return DeviceAddress<T>(address.raw + (uint64_t)(count * (uint)sizeof(T)));
#else
    uint encodedSlot = (uint)(address.raw >> 32);
    uint64_t offset = (uint64_t)(address.raw & 0xFFFFFFFFu) + (uint64_t)count * (uint64_t)sizeof(T);
    if (offset > 0xFFFFFFFFu)
        return DeviceAddress<T>(0);
    return DeviceAddress<T>(((uint64_t)encodedSlot << 32) | (uint)offset);
#endif
}

[ForceInline]
public DeviceAddress<T> operator+<T>(DeviceAddress<T> address, int count)
{
    if (address.raw == 0)
        return address;
#if defined(__TARGET_SPIRV__)
    return DeviceAddress<T>(address.raw + (uint64_t)((int64_t)count * (int64_t)sizeof(T)));
#else
    uint encodedSlot = (uint)(address.raw >> 32);
    int64_t offset = (int64_t)(address.raw & 0xFFFFFFFFu) + (int64_t)count * (int64_t)sizeof(T);
    if (offset < 0 || offset > 0xFFFFFFFFll)
        return DeviceAddress<T>(0);
    return DeviceAddress<T>(((uint64_t)encodedSlot << 32) | (uint)offset);
#endif
}

public extension DeviceAddress<uint>
{
    [ForceInline]
    public uint atomicAdd(uint index, uint value)
    {
        uint original = 0;
#if defined(__TARGET_SPIRV__)
        InterlockedAdd(((uint*)this.raw)[index], value, original);
#else
        uint encodedSlot = (uint)(this.raw >> 32);
        uint offset = (uint)(this.raw & 0xFFFFFFFFu) + index * (uint)sizeof(uint);
        GetD3D12RWBuffer(encodedSlot).InterlockedAdd(offset, value, original);
#endif
        return original;
    }
}
