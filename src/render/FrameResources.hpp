// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "core/FrameStorage.hpp"
#include <Zahlen/Render/Handles.hpp>
#include <cstdint>

namespace ZHLN {

struct FrameResources {
    static constexpr uint32_t kJointBufferRingSize = Vk::kFramesInFlight + 1;

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

    // Three-slot ring: while GPU frame N-1 reads slots {prev(N-1), curr(N-1)},
    // CPU frame N writes the third slot without clobbering either.
    std::array<Vk::Buffer, kJointBufferRingSize> jointBuffers;
    uint32_t                                     currentJointSlot  = 0;
    uint32_t                                     previousJointSlot = kJointBufferRingSize - 1;
    PerFrame<Vk::AccelerationStructure>          tlas;

    [[nodiscard]] auto CurrentJointBuffer() noexcept -> Vk::Buffer& {
        return jointBuffers[currentJointSlot];
    }
    [[nodiscard]] auto CurrentJointBuffer() const noexcept -> const Vk::Buffer& {
        return jointBuffers[currentJointSlot];
    }
    [[nodiscard]] auto PreviousJointBuffer() const noexcept -> const Vk::Buffer& {
        return jointBuffers[previousJointSlot];
    }
    void AdvanceJointRing() noexcept {
        previousJointSlot = currentJointSlot;
        currentJointSlot  = (currentJointSlot + 1) % kJointBufferRingSize;
    }

    // One frame's slice of every PerFrame<> above: the storage's own names,
    // bound to the slot this frame occupies. Built once when the frame begins,
    // so a pass asks for `frame.lightStorageBuffers` instead of walking into the
    // renderer, finding the frames-in-flight storage, and indexing it with a
    // slot number it had to fetch from presentation state first.
    //
    // Lives next to the storage, so a new per-frame buffer is a two-line change
    // in one place.
    //
    // The joint buffers are not PerFrame<>: they are a separate three-slot ring
    // with its own cursor (AdvanceJointRing, which stays on the renderer because
    // it is the renderer that advances between frames). The slice resolves the
    // two slots this frame reads and writes.
    struct Slice {
        Slice(FrameResources& resources, uint32_t index) noexcept:
            lineVbos(resources.lineVbos[index]), lineVboAddresses(resources.lineVboAddresses[index]), clusterGridBuffers(resources.clusterGridBuffers[index]),
            lightIndexListBuffers(resources.lightIndexListBuffers[index]), globalCounterBuffers(resources.globalCounterBuffers[index]),
            frameUniformBuffers(resources.frameUniformBuffers[index]), lightStorageBuffers(resources.lightStorageBuffers[index]),
            shadowTileBuffers(resources.shadowTileBuffers[index]), instanceDataBuffers(resources.instanceDataBuffers[index]),
            indirectCommandsBuffers(resources.indirectCommandsBuffers[index]), indirectCommandsBuffersPass2(resources.indirectCommandsBuffersPass2[index]),
            secondPassCandidatesBuffers(resources.secondPassCandidatesBuffers[index]), secondPassCountBuffers(resources.secondPassCountBuffers[index]),
            tlas(resources.tlas[index]), tlasAddresses(resources.tlasAddresses[index]), debugMeshHandles(resources.debugMeshHandles[index]),
            currentJoint(resources.jointBuffers[resources.currentJointSlot]), previousJoint(resources.jointBuffers[resources.previousJointSlot]) {
        }

        Vk::Buffer&                lineVbos;
        VkDeviceAddress&           lineVboAddresses;
        Vk::Buffer&                clusterGridBuffers;
        Vk::Buffer&                lightIndexListBuffers;
        Vk::Buffer&                globalCounterBuffers;
        Vk::Buffer&                frameUniformBuffers;
        Vk::Buffer&                lightStorageBuffers;
        Vk::Buffer&                shadowTileBuffers;
        Vk::Buffer&                instanceDataBuffers;
        Vk::Buffer&                indirectCommandsBuffers;
        Vk::Buffer&                indirectCommandsBuffersPass2;
        Vk::Buffer&                secondPassCandidatesBuffers;
        Vk::Buffer&                secondPassCountBuffers;
        Vk::AccelerationStructure& tlas;
        Vk::DeviceAddress<>&       tlasAddresses;
        BufferHandle&              debugMeshHandles;
        Vk::Buffer&                currentJoint;
        Vk::Buffer&                previousJoint;
    };

    PerFrame<Vk::DeviceAddress<>>       tlasAddresses;
    PerFrame<Vk::Buffer>                tlasBuffer;
    PerFrame<Vk::Buffer>                tlasScratchBuffer;
    PerFrame<Vk::Buffer>                tlasInstanceBuffers;
    PerFrame<BufferHandle>              debugMeshHandles;

    void Destroy(Vk::Allocator& allocator) noexcept {
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

    template <typename... Owned>
    static void DestroyAll(Vk::Allocator& allocator, Owned&... owned) noexcept {
        (DestroyOne(allocator, owned), ...);
    }
};

} // namespace ZHLN
