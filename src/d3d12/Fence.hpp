#pragma once

// Timeline synchronization. ID3D12Fence is the 1:1 analogue of the Vulkan
// timeline semaphore: the same monotonic uint64 value space, signaled on
// a queue at submission and waited on by CPU (SetEventOnCompletion) or by
// another queue (ID3D12CommandQueue::Wait). FrameSync<N> mirrors
// Vk::FrameSync<N>'s slot discipline with two fences per context instead
// of two semaphores per slot -- fences are cheap and value-addressed, so
// one graphics + one compute fence serves every slot; only the submitted
// values are per-slot state.

#include "Com.hpp"
#include "D3D12Error.hpp"

#include <array>
#include <cstdint>
#include <expected>

namespace ZHLN::D3D12 {

class TimelineFence {
  public:
    TimelineFence() noexcept = default;
    ~TimelineFence() noexcept;

    TimelineFence(const TimelineFence&)                    = delete;
    auto operator=(const TimelineFence&) -> TimelineFence& = delete;
    TimelineFence(TimelineFence&&) noexcept;
    auto operator=(TimelineFence&&) noexcept -> TimelineFence&;

    [[nodiscard]] static auto Create(ID3D12Device* device) noexcept -> std::expected<TimelineFence, ErrorCode>;

    // Enqueue a signal of the next value on `queue`; returns the value
    // signaled (0 on failure, which no real submission can produce since
    // values start at 1). The returned value is the wait token for
    // Wait()/WaitOnQueue() and for cross-queue dependencies.
    [[nodiscard]] auto Signal(ID3D12CommandQueue* queue) noexcept -> uint64_t;

    // Block the calling thread until the fence reaches `value`.
    [[nodiscard]] auto Wait(uint64_t value, uint32_t timeoutMs = 0xFFFFFFFFu) const noexcept -> std::expected<void, ErrorCode>;

    // Enqueue a GPU-side wait: `queue` stalls until the fence reaches
    // `value`. This is how a graphics submission depends on compute work
    // without a CPU round-trip (the analogue of a semaphore wait stage).
    void WaitOnQueue(ID3D12CommandQueue* queue, uint64_t value) const noexcept;

    [[nodiscard]] auto CompletedValue() const noexcept -> uint64_t;
    [[nodiscard]] auto Get() const noexcept -> ID3D12Fence* {
        return _fence.Get();
    }
    [[nodiscard]] auto NextValue() const noexcept -> uint64_t {
        return _next;
    }
    explicit operator bool() const noexcept {
        return _fence != nullptr;
    }

  private:
    ComPtr<ID3D12Fence> _fence;
    // Opaque OS event handle (HANDLE on Windows), stored as void* so this
    // header does not need <windows.h>; created alongside the fence.
    void*    _event = nullptr;
    uint64_t _next  = 0;
};

template <uint32_t N>
    requires(N > 0)
class FrameSync {
  public:
    FrameSync() noexcept = default;

    FrameSync(const FrameSync&)                    = delete;
    auto operator=(const FrameSync&) -> FrameSync& = delete;
    FrameSync(FrameSync&&) noexcept                = default;
    auto operator=(FrameSync&&) noexcept -> FrameSync& = default;

    [[nodiscard]] static auto Create(ID3D12Device* device) noexcept -> std::expected<FrameSync, ErrorCode> {
        auto graphics = TimelineFence::Create(device);
        if (!graphics) {
            return std::unexpected(graphics.error());
        }
        auto compute = TimelineFence::Create(device);
        if (!compute) {
            return std::unexpected(compute.error());
        }
        FrameSync sync;
        sync._graphics = std::move(*graphics);
        sync._compute  = std::move(*compute);
        return sync;
    }

    // Signal the graphics fence for this slot's submission; returns the
    // timeline value to correlate with WaitSlotIdle / cross-queue waits.
    [[nodiscard]] auto SignalGraphics(ID3D12CommandQueue* queue, uint32_t slot) noexcept -> uint64_t {
        const uint64_t value       = _graphics.Signal(queue);
        _graphicsSubmitted[slot % N] = value;
        return value;
    }

    [[nodiscard]] auto SignalCompute(ID3D12CommandQueue* queue, uint32_t slot) noexcept -> uint64_t {
        const uint64_t value       = _compute.Signal(queue);
        _computeSubmitted[slot % N] = value;
        return value;
    }

    // Block until both queues' work for this slot has completed -- the
    // frame-slot reuse gate, same role as Vk::FrameSync::WaitSlotIdle.
    [[nodiscard]] auto WaitSlotIdle(uint32_t slot) const noexcept -> std::expected<void, ErrorCode> {
        const uint32_t s = slot % N;
        if (const uint64_t value = _graphicsSubmitted[s]; value != 0) {
            if (auto result = _graphics.Wait(value); !result) {
                return result;
            }
        }
        if (const uint64_t value = _computeSubmitted[s]; value != 0) {
            if (auto result = _compute.Wait(value); !result) {
                return result;
            }
        }
        return {};
    }

    // GPU-side dependency: make `queue` wait for this slot's compute /
    // graphics submission before executing further work.
    void WaitForComputeOn(ID3D12CommandQueue* queue, uint32_t slot) const noexcept {
        if (const uint64_t value = _computeSubmitted[slot % N]; value != 0) {
            _compute.WaitOnQueue(queue, value);
        }
    }
    void WaitForGraphicsOn(ID3D12CommandQueue* queue, uint32_t slot) const noexcept {
        if (const uint64_t value = _graphicsSubmitted[slot % N]; value != 0) {
            _graphics.WaitOnQueue(queue, value);
        }
    }

    [[nodiscard]] auto GraphicsFence() noexcept -> TimelineFence& {
        return _graphics;
    }
    [[nodiscard]] auto ComputeFence() noexcept -> TimelineFence& {
        return _compute;
    }

  private:
    TimelineFence              _graphics;
    TimelineFence              _compute;
    std::array<uint64_t, N>    _graphicsSubmitted {};
    std::array<uint64_t, N>    _computeSubmitted {};
};

} // namespace ZHLN::D3D12
