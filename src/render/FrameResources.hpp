// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Physical resources owned by an in-flight GPU frame, one slot per frame in
// flight. Selection is explicit; see PerFrame in core/FrameStorage.hpp.
//
// This struct owns its own teardown. It used to be `RenderContext::Impl::
// PerFrameResources`, a bag of members whose destroy list lived ~600 lines
// away in `~RenderContext::Impl` as seventeen consecutive calls -- so adding a
// member meant editing two files, and forgetting the second leaked a buffer
// per frame slot. `Destroy` is now the only list, and it is next to the
// declarations it destroys.

#include "Rendering.hpp"
#include "core/FrameStorage.hpp"
#include <Zahlen/Render/Handles.hpp>
#include <cstdint>

namespace ZHLN {

struct FrameResources {
    PerFrame<Vk::Buffer>      lineVbos;
    PerFrame<VkDeviceAddress> lineVboAddresses;
    PerFrame<Vk::Buffer>      clusterGridBuffers;
    PerFrame<Vk::Buffer>      lightIndexListBuffers;
    PerFrame<Vk::Buffer>      globalCounterBuffers;
    PerFrame<Vk::Buffer>      frameUniformBuffers;
    PerFrame<Vk::Buffer>      lightStorageBuffers;
    PerFrame<Vk::Buffer>      shadowTileBuffers;
    PerFrame<Vk::Buffer>      instanceDataBuffers;
    PerFrame<Vk::Buffer>      indirectCommandsBuffers;
    PerFrame<Vk::Buffer>      indirectCommandsBuffersPass2;
    PerFrame<Vk::Buffer>      secondPassCandidatesBuffers;
    PerFrame<Vk::Buffer>      secondPassCountBuffers;
    // Joint data is addressed by both the current and previous frame slot.
    // Its physical lifetime follows the N-slot frame ring, not PingPong.
    PerFrame<Vk::Buffer>                jointBuffers;
    PerFrame<Vk::AccelerationStructure> tlas;
    // The address is the TLAS's only identity outside the build: passes,
    // descriptors and instances consume it, never the object handle.
    // Cached at creation (BuildTLAS builds into the same objects, so
    // these stay valid for the objects' whole lifetime).
    PerFrame<Vk::DeviceAddress<>>       tlasAddresses;
    PerFrame<Vk::Buffer>                tlasBuffer;
    PerFrame<Vk::Buffer>                tlasScratchBuffer;
    PerFrame<Vk::Buffer>                tlasInstanceBuffers;
    PerFrame<BufferHandle>              debugMeshHandles;

    // Releases every allocation this struct owns, in the order the driver
    // requires: acceleration structures first, then the VMA buffers backing
    // them, then everything else.
    //
    // Three members are deliberately absent, because this struct does not own
    // what they name: `lineVboAddresses` and `tlasAddresses` are plain device
    // addresses, and `debugMeshHandles` belongs to GeometryManager, which
    // releases it in its own RetireAll.
    void Destroy(Vk::Allocator& allocator) noexcept {
        // Acceleration structures must go before their backing VMA buffers:
        // the handle is what keeps the driver's copy of the build alive.
        for (auto& accel: tlas) {
            accel = Vk::AccelerationStructure {};
        }
        for (auto& address: tlasAddresses) {
            address = nullptr;
        }

        DestroyAll(
            allocator, lineVbos, clusterGridBuffers, lightIndexListBuffers, globalCounterBuffers, frameUniformBuffers, lightStorageBuffers,
            shadowTileBuffers, instanceDataBuffers, indirectCommandsBuffers, indirectCommandsBuffersPass2, secondPassCandidatesBuffers,
            secondPassCountBuffers, jointBuffers, tlasBuffer, tlasScratchBuffer, tlasInstanceBuffers
        );
    }

  private:
    template <typename Owned>
    static void DestroyOne(Vk::Allocator& allocator, Owned& perFrame) noexcept {
        for (auto& resource: perFrame) {
            allocator.DestroyBuffer(resource);
        }
    }

    // The comma fold is what fixes the order: every element is evaluated
    // left to right, so the sequence below is the sequence the allocator
    // sees.
    template <typename... Owned>
    static void DestroyAll(Vk::Allocator& allocator, Owned&... owned) noexcept {
        (DestroyOne(allocator, owned), ...);
    }
};

} // namespace ZHLN
