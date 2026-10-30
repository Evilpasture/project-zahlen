// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace ZHLN::Test {

// A caller-owned allocator for checking allocation churn and lifetime through
// a system's public memory-resource constructor. Never inspects that system's
// buffers; the upstream resource provides the actual storage and alignment.
class TrackingMemoryResource final : public std::pmr::memory_resource {
  public:
    explicit TrackingMemoryResource(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource()) noexcept:
        _upstream(upstream != nullptr ? upstream : std::pmr::new_delete_resource()) {
    }

    [[nodiscard]] auto Allocations() const noexcept -> size_t {
        return _allocations.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto Deallocations() const noexcept -> size_t {
        return _deallocations.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto LiveBlocks() const noexcept -> size_t {
        return _liveBlocks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto LiveBytes() const noexcept -> size_t {
        return _liveBytes.load(std::memory_order_relaxed);
    }

  private:
    auto do_allocate(size_t bytes, size_t alignment) -> void* override {
        void* const memory = _upstream->allocate(bytes, alignment);
        _allocations.fetch_add(1, std::memory_order_relaxed);
        _liveBlocks.fetch_add(1, std::memory_order_relaxed);
        _liveBytes.fetch_add(bytes, std::memory_order_relaxed);
        return memory;
    }

    void do_deallocate(void* memory, size_t bytes, size_t alignment) override {
        _upstream->deallocate(memory, bytes, alignment);
        _deallocations.fetch_add(1, std::memory_order_relaxed);
        _liveBlocks.fetch_sub(1, std::memory_order_relaxed);
        _liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }

    std::pmr::memory_resource* _upstream;
    std::atomic<size_t>       _allocations {0};
    std::atomic<size_t>       _deallocations {0};
    std::atomic<size_t>       _liveBlocks {0};
    std::atomic<size_t>       _liveBytes {0};
};

} // namespace ZHLN::Test
