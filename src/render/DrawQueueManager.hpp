// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/DrawQueueManager.hpp
//
// The frame's draw submission: the six queues the engine pushes into, and the
// CPU-side sort that orders the main queue before the passes read it.
//
// Pure CPU. There is no device, no allocator and no command buffer anywhere in
// here, which is why it takes no injected dependencies and why it can be
// exercised without a Vulkan device. The one thing it deliberately does not own
// is the upload of what the queues describe: turning lineQueue into vertices in
// a mapped VBO needs the line pipeline and the frame's double-buffered arrays,
// and injecting those into a queue container would drag frame-buffer management
// and pipeline state into it. That stays in PrepareSceneFrame (RenderFrame.cpp),
// which reads Lines() and does the mapping itself.
//
// The queues are handed out by reference rather than through Push/Pop methods
// because the passes both read and mutate them in bulk -- the GPU-culling path
// resizes the draw queue to its instance budget, and ClearDrawQueues clears two
// of the six -- so an accessor per queue is the honest surface and a method per
// operation would be a longer way to spell the same thing.

#pragma once
#include "DrawCommands.hpp"

#include <Zahlen/Core/RadixSort.hpp>

namespace ZHLN {

class DrawQueueManager {
  public:
    DrawQueueManager()                                       = default;
    ~DrawQueueManager()                                      = default;
    DrawQueueManager(const DrawQueueManager&)                = default;
    auto operator=(const DrawQueueManager&) -> DrawQueueManager& = default;
    DrawQueueManager(DrawQueueManager&&) noexcept            = default;
    auto operator=(DrawQueueManager&&) noexcept -> DrawQueueManager& = default;

    // --- The queues --------------------------------------------------------
    // Mutable on purpose: the passes resize and clear these directly.

    [[nodiscard]] auto Draws() noexcept -> ZHLN::Array<DrawCommand>& { return _queues.drawQueue; }
    [[nodiscard]] auto Draws() const noexcept -> const ZHLN::Array<DrawCommand>& { return _queues.drawQueue; }

    [[nodiscard]] auto CsgDraws() noexcept -> ZHLN::Array<CSGDrawCommand>& { return _queues.csgDrawQueue; }
    [[nodiscard]] auto CsgDraws() const noexcept -> const ZHLN::Array<CSGDrawCommand>& { return _queues.csgDrawQueue; }

    [[nodiscard]] auto ParticleEmitters() noexcept -> ZHLN::Array<ParticleEmitterCommand>& { return _queues.particleEmittersQueue; }
    [[nodiscard]] auto ParticleEmitters() const noexcept -> const ZHLN::Array<ParticleEmitterCommand>& { return _queues.particleEmittersQueue; }

    [[nodiscard]] auto MeshParticleEmitters() noexcept -> ZHLN::Array<MeshParticleEmitterCommand>& { return _queues.meshParticleQueue; }
    [[nodiscard]] auto MeshParticleEmitters() const noexcept -> const ZHLN::Array<MeshParticleEmitterCommand>& { return _queues.meshParticleQueue; }

    [[nodiscard]] auto Decals() noexcept -> ZHLN::Array<DecalDrawCommand>& { return _queues.decalQueue; }
    [[nodiscard]] auto Decals() const noexcept -> const ZHLN::Array<DecalDrawCommand>& { return _queues.decalQueue; }

    [[nodiscard]] auto Lines() noexcept -> ZHLN::Array<LineSegment>& { return _queues.lineQueue; }
    [[nodiscard]] auto Lines() const noexcept -> const ZHLN::Array<LineSegment>& { return _queues.lineQueue; }

    // --- Frame lifecycle ---------------------------------------------------

    // Orders Draws() by material then mesh, so the passes change pipeline state
    // as little as possible. Runs after submission is complete and before the
    // scene passes read the queue. The scratch buffers are members rather than
    // locals so a frame does not reallocate three arrays the size of the queue.
    void Sort();

    // Empties all six. Keeps the allocations, which is the point: this runs
    // every EndFrame.
    //
    // Spelled out rather than driven by reflection. This used to be
    // Reflect::ForEachField over the six members, which meant a build without
    // generated descriptors visited zero fields and cleared nothing -- silently,
    // leaving the previous frame's draws to be submitted again. A frame
    // lifecycle function that can fail by doing nothing is not worth the six
    // lines it saves.
    void Clear() noexcept {
        _queues.drawQueue.clear();
        _queues.csgDrawQueue.clear();
        _queues.particleEmittersQueue.clear();
        _queues.meshParticleQueue.clear();
        _queues.decalQueue.clear();
        _queues.lineQueue.clear();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return _queues.drawQueue.empty() && _queues.csgDrawQueue.empty() && _queues.particleEmittersQueue.empty() && _queues.meshParticleQueue.empty() &&
               _queues.decalQueue.empty() && _queues.lineQueue.empty();
    }

  private:
    RenderQueues _queues;

    // Sort working space: the keyed items, the ping-pong buffer RadixSort64
    // writes into, and the gather target the sorted commands are collected into
    // before being swapped with the queue.
    ZHLN::Array<SortItem>    _sortItems;
    ZHLN::Array<SortItem>    _sortTemp;
    ZHLN::Array<DrawCommand> _sorted;
};

} // namespace ZHLN
