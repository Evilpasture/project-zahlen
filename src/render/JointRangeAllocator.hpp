// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZHLN {

inline constexpr uint32_t kDefaultJointPaletteCapacity = 65536;

// Renderer-owned GPU joint-palette index allocator. Manages only
// {offset,count,refCount} intervals into the joint-matrix buffers: no
// physics, no blend weights. Released ranges are NOT reusable until the
// renderer has reset them to Identity in every joint-buffer ring slot
// (see ClearRetiringForSlot, driven by RenderContext::BeginFrame after
// WaitSlotIdle proves the slot GPU-idle); no engine frame hook is involved.
class JointRangeAllocator {
  public:
    explicit JointRangeAllocator(uint32_t capacity = kDefaultJointPaletteCapacity) noexcept: _capacity(capacity) {}
    JointRangeAllocator(const JointRangeAllocator&) = delete;
    auto operator=(const JointRangeAllocator&) -> JointRangeAllocator& = delete;
    JointRangeAllocator(JointRangeAllocator&& o) noexcept:
        _capacity(o._capacity), _next(o._next), _live(std::move(o._live)), _free(std::move(o._free)), _retiring(std::move(o._retiring)) { o._next = 0; }
    auto operator=(JointRangeAllocator&& o) noexcept -> JointRangeAllocator& {
        if (this != &o) {
            ZHLN::MutexGuard guard(_mutex);
            _capacity = o._capacity; _next = o._next; _live = std::move(o._live); _free = std::move(o._free); _retiring = std::move(o._retiring); o._next = 0;
        }
        return *this;
    }
    [[nodiscard]] auto Capacity() const noexcept -> uint32_t { return _capacity; }

    [[nodiscard]] auto Allocate(uint32_t count) noexcept -> uint32_t {
        if (count == 0 || count > _capacity) return UINT32_MAX;
        ZHLN::MutexGuard guard(_mutex);
        for (auto it = _free.begin(); it != _free.end(); ++it) {
            if (it->count >= count) {
                const auto offset = it->offset;
                if (it->count == count) _free.erase(it);
                else { it->offset += count; it->count -= count; }
                _live[offset] = {offset, count, 1};
                return offset;
            }
        }
        if (_next > _capacity - count) return UINT32_MAX;
        const auto offset = _next;
        _next += count;
        _live[offset] = {offset, count, 1};
        return offset;
    }
    // One more SkeletalMeshComponent shares this live range.
    [[nodiscard]] auto Retain(uint32_t offset) noexcept -> bool {
        ZHLN::MutexGuard guard(_mutex);
        auto it = _live.find(offset);
        if (it == _live.end()) return false;
        ++it->second.refCount;
        return true;
    }
    // Drop one ref; on the last ref the range retires (not yet reusable).
    void Release(uint32_t offset, uint32_t count) noexcept {
        if (count == 0) return;
        ZHLN::MutexGuard guard(_mutex);
        auto it = _live.find(offset);
        if (it == _live.end() || it->second.count != count) return;
        if (--it->second.refCount > 0) return;
        _live.erase(it);
        _retiring.push_back({offset, count, 0});
    }
    struct RetiringRange { uint32_t offset; uint32_t count; uint32_t clearedMask; };
    [[nodiscard]] auto HasRetiring() noexcept -> bool { ZHLN::MutexGuard guard(_mutex); return !_retiring.empty(); }
    // One locked pass over retiring ranges for ring slot `slot`: invokes
    // clearRange(offset,count) only for ranges not yet cleared in this slot,
    // then promotes ranges cleared in every slot to the free list. The
    // renderer maps CurrentJointBuffer() once and passes the write lambda,
    // so there is no snapshot allocation and no snapshot/promote race.
    template <typename Fn>
    void ClearRetiringForSlot(uint32_t slot, uint32_t ringSize, Fn&& clearRange) noexcept {
        ZHLN::MutexGuard guard(_mutex);
        if (_retiring.empty()) return;
        const uint32_t bit = 1u << slot;
        const uint32_t all = (1u << ringSize) - 1u;
        bool promoted = false;
        for (size_t i = 0; i < _retiring.size();) {
            auto& r = _retiring[i];
            if ((r.clearedMask & bit) == 0u) {
                clearRange(r.offset, r.count);
                r.clearedMask |= bit;
            }
            if (r.clearedMask == all) {
                _free.push_back({r.offset, r.count, 0});
                _retiring[i] = _retiring.back();
                _retiring.pop_back();
                promoted = true;
            } else {
                ++i;
            }
        }
        if (!promoted) return;
        std::sort(_free.begin(), _free.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
        std::vector<Range> merged;
        merged.reserve(_free.size());
        for (const auto& r : _free) {
            if (!merged.empty() && merged.back().offset + merged.back().count == r.offset) merged.back().count += r.count;
            else merged.push_back(r);
        }
        _free = std::move(merged);
    }
  private:
    struct Range { uint32_t offset; uint32_t count; uint32_t refCount = 0; };
    uint32_t _capacity;
    uint32_t _next = 0;
    ZHLN::Mutex _mutex;
    std::unordered_map<uint32_t, Range> _live; // keyed by offset: O(1) Retain/Release
    std::vector<Range> _free;
    std::vector<RetiringRange> _retiring;
};
} // namespace ZHLN
