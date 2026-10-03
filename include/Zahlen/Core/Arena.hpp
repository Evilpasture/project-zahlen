// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace ZHLN {

// A bump allocator over one page-backed block.
//
// The lifetime it models is "everything in here dies together": allocations go
// forward from the last one and Reset() hands the whole block back by dropping
// the offset to zero. Nothing is freed individually -- there is no per-allocation
// header to find, no size class to pick, and a deallocation is one store. That is
// the right shape for anything that is rebuilt every frame (system scratch, this
// frame's component copies) and the wrong shape for anything that outlives a
// phase, which is why the caller, not the arena, owns the phase.
//
// Over-aligned requests are honoured by bumping the offset, not by wasting a
// page: the block itself starts page-aligned (AllocatePages), so any alignment
// a page can be divided by is satisfiable inside it.
class LinearArena {
  public:
    // `capacity` is the number of usable bytes, in the caller's terms. The
    // mapping is page-rounded above it, but the surplus is deliberately not
    // published: Allocate() refuses at `capacity`, so an arena sized N never
    // depends on which page size the host has.
    explicit LinearArena(size_t capacity) noexcept: _memory(static_cast<std::byte*>(AllocatePages(capacity))), _capacity(capacity) {
        // Always-on, where the alignment check below is a dev-only Assert: not
        // being able to map the block is the machine refusing, not a caller
        // precondition to be optimized away, and every later call dereferences
        // _memory.
        if (_memory == nullptr) {
            Panic("LinearArena could not map {} bytes of backing pages", capacity);
        }
    }

    ~LinearArena() noexcept {
        FreePages(_memory, _capacity);
    }

    LinearArena(const LinearArena&)                    = delete;
    auto operator=(const LinearArena&) -> LinearArena& = delete;
    LinearArena(LinearArena&& other) noexcept:
        _memory(std::exchange(other._memory, nullptr)), _capacity(std::exchange(other._capacity, 0)), _offset(std::exchange(other._offset, 0)) {
    }

    auto operator=(LinearArena&& other) noexcept -> LinearArena& {
        if (this != &other) {
            FreePages(_memory, _capacity);
            _memory   = std::exchange(other._memory, nullptr);
            _capacity = std::exchange(other._capacity, 0);
            _offset   = std::exchange(other._offset, 0);
        }
        return *this;
    }

    // Bytes are undefined on return and stay live until Reset(), so a caller
    // that needs a destructor run has to run it itself -- the arena has nowhere
    // to remember one.
    [[nodiscard]] auto Allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept -> void* {
        Assert(alignment <= GetPageSize(), "LinearArena is page-backed: it cannot honour an alignment larger than a page");
        const size_t aligned = Math::AlignUp(_offset, alignment);
        // A hard refusal rather than a fallback to the heap: capacity is part of
        // the caller's design (a frame's worth of something), so running out is
        // a number to raise, not a case to paper over. Always-on for the same
        // reason as the map failure above, and in the same shape the ECS uses for
        // memory-safety refusals (SparseSet's resize violations): an overrun
        // writes past the mapping, so it must not collapse to [[assume(false)]]
        // the way a dev-only Assert does. It is one branch on the hot path -- the
        // message is formatted only when the branch is taken.
        if (aligned + bytes > _capacity) {
            Panic("LinearArena is out of capacity: {} bytes at offset {} of {}", bytes, aligned, _capacity);
        }
        _offset = aligned + bytes;
        return _memory + aligned;
    }

    void Reset() noexcept {
        _offset = 0;
    }

    [[nodiscard]] auto AllocatedBytes() const noexcept -> size_t {
        return _offset;
    }

    [[nodiscard]] auto Capacity() const noexcept -> size_t {
        return _capacity;
    }

  private:
    std::byte* _memory   = nullptr;
    size_t     _capacity = 0;
    size_t     _offset   = 0;
};

// One LinearArena per worker index.
//
// A per-worker arena is what makes the scratch free of synchronisation: a worker
// allocates only from its own block, so handing out scratch needs no lock and an
// allocation cannot interleave with another worker's. ResetAll() is the other
// half of the contract -- it must run where no worker is inside a frame (the
// frame step that starts a graph, never a system), because it invalidates every
// pointer the last frame's systems were handed.
class WorkerScratchPool {
  public:
    WorkerScratchPool(size_t perWorkerCapacity, uint32_t workerCount): _perWorkerCapacity(perWorkerCapacity) {
        const uint32_t count = std::max(workerCount, 1u);
        _arenas.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            _arenas.emplace_back(perWorkerCapacity);
        }
    }

    WorkerScratchPool(const WorkerScratchPool&)                    = delete;
    auto operator=(const WorkerScratchPool&) -> WorkerScratchPool& = delete;
    WorkerScratchPool(WorkerScratchPool&&)                         = delete;
    auto operator=(WorkerScratchPool&&) -> WorkerScratchPool&      = delete;

    [[nodiscard]] auto GetWorkerArena(uint32_t workerIndex) noexcept -> LinearArena& {
        Assert(workerIndex < _arenas.size(), "Worker index is outside the scratch pool: the pool was sized for a different worker count");
        return _arenas[workerIndex];
    }

    void ResetAll() noexcept {
        for (auto& arena: _arenas) {
            arena.Reset();
        }
    }

    [[nodiscard]] auto WorkerCount() const noexcept -> size_t {
        return _arenas.size();
    }

    [[nodiscard]] auto PerWorkerCapacity() const noexcept -> size_t {
        return _perWorkerCapacity;
    }

  private:
    std::vector<LinearArena> _arenas;
    size_t                   _perWorkerCapacity = 0;
};

} // namespace ZHLN
