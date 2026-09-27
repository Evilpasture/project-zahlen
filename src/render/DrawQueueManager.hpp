// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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


    void Sort();

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

    ZHLN::Array<SortItem>    _sortItems;
    ZHLN::Array<SortItem>    _sortTemp;
    ZHLN::Array<DrawCommand> _sorted;
};

}
