// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Array.hpp>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

namespace ZHLN {

// A non-owning STL allocator adapter for LinearArena.
//
// Containers still construct and destroy their elements normally; only storage
// release changes. With an arena bound, deallocate is intentionally a no-op, so
// growth leaves old buffers in the arena until Reset() -- reserve when the final
// size is known. The arena must outlive the container and every element in its
// storage, and reset only after those containers have been destroyed. A
// default-constructed allocator has no arena and uses DefaultAllocator<T>, which
// is useful when scratch is not tied to an arena lifetime.
template <typename T>
class ArenaAllocator {
  public:
    using value_type                         = T;
    using is_always_equal                    = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap             = std::true_type;

    template <typename U>
    struct rebind {
        using other = ArenaAllocator<U>;
    };

    constexpr ArenaAllocator() noexcept = default;

    explicit constexpr ArenaAllocator(LinearArena& arena) noexcept: _arena(&arena) {
    }

    explicit constexpr ArenaAllocator(LinearArena* arena) noexcept: _arena(arena) {
    }

    template <typename U>
    constexpr ArenaAllocator(const ArenaAllocator<U>& other) noexcept: _arena(other.GetArena()) {
    }

    [[nodiscard]] auto allocate(size_t count) -> T* {
        if (count == 0) {
            return nullptr;
        }
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            Panic("ArenaAllocator request overflows size_t: {} elements of {} bytes", count, sizeof(T));
        }

        if (_arena == nullptr) {
            return _fallback.allocate(count);
        }
        if (alignof(T) > GetPageSize()) {
            Panic("ArenaAllocator cannot satisfy alignment {} (page size {})", alignof(T), GetPageSize());
        }
        return static_cast<T*>(_arena->Allocate(count * sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, size_t count) noexcept {
        if (_arena == nullptr) {
            _fallback.deallocate(pointer, count);
        }
        // Arena storage is reclaimed as a whole by LinearArena::Reset().
    }

    [[nodiscard]] constexpr auto GetArena() const noexcept -> LinearArena* {
        return _arena;
    }

    template <typename U>
    friend constexpr auto operator==(const ArenaAllocator& lhs, const ArenaAllocator<U>& rhs) noexcept -> bool {
        return lhs._arena == rhs.GetArena();
    }

  private:
    template <typename>
    friend class ArenaAllocator;

    LinearArena*    _arena = nullptr;
    DefaultAllocator<T> _fallback {};
};

// The standard-vector spelling used for frame/task scratch. The vector keeps
// its normal object lifetime semantics while its backing bytes come from an
// explicitly supplied LinearArena.
template <typename T>
using ScratchVector = std::vector<T, ArenaAllocator<T>>;

// The ZHLN::Array equivalent. InlineCap defaults to zero so all storage is
// governed by the selected allocator; callers can request inline storage when
// that is a better fit for their value type.
template <typename T, size_t InlineCap = 0>
using ArenaArray = Array<T, InlineCap, ArenaAllocator<T>>;

} // namespace ZHLN
