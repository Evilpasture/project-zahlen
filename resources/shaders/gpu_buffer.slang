// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module gpu_buffer;

// A typed view over a buffer device address. This owns neither the allocation
// nor its lifetime, has no bounds information, and must not be dereferenced
// when the address is zero. Host-facing push/instance/heap fields stay uint64_t.
public struct GPUBuffer<T> {
    public uint64_t address;

    public __init(uint64_t address) {
        this.address = address;
    }

    [ForceInline]
    public bool isValid() {
        return this.address != 0;
    }

    [ForceInline]
    public T load(uint index) {
        return ((T*) this.address)[index];
    }

    [ForceInline]
    public void store(uint index, T value) {
        ((T*) this.address)[index] = value;
    }
}

// The compaction counter needs an l-value for InterlockedAdd. Keep even that
// pointer conversion inside the typed buffer instead of exposing a raw T*.
public extension GPUBuffer<uint> {
    [ForceInline]
    public uint atomicAdd(uint index, uint value) {
        uint previous = 0;
        InterlockedAdd(((uint*) this.address)[index], value, previous);
        return previous;
    }
}
