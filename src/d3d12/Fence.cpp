// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fence.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ZHLN::D3D12 {

TimelineFence::~TimelineFence() noexcept {
#if defined(_WIN32)
    if (_event != nullptr) {
        CloseHandle(static_cast<HANDLE>(_event));
        _event = nullptr;
    }
#endif
}

TimelineFence::TimelineFence(TimelineFence&& other) noexcept
    : _fence(std::move(other._fence)), _event(std::exchange(other._event, nullptr)), _next(other._next) {
}

auto TimelineFence::operator=(TimelineFence&& other) noexcept -> TimelineFence& {
    if (this != &other) {
#if defined(_WIN32)
        if (_event != nullptr) {
            CloseHandle(static_cast<HANDLE>(_event));
        }
#endif
        _fence = std::move(other._fence);
        _event = std::exchange(other._event, nullptr);
        _next  = other._next;
    }
    return *this;
}

auto TimelineFence::Create(ID3D12Device* device) noexcept -> std::expected<TimelineFence, ErrorCode> {
    TimelineFence fence;
    const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(fence._fence.AddressOf()));
    if (FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
#if defined(_WIN32)
    fence._event = static_cast<void*>(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (fence._event == nullptr) {
        return std::unexpected(ErrorCode {D3D12Error::FenceCreationFailed});
    }
#endif
    return fence;
}

auto TimelineFence::Signal(ID3D12CommandQueue* queue) noexcept -> uint64_t {
    const uint64_t value = ++_next;
    if (FAILED(queue->Signal(_fence.Get(), value))) {
        --_next;
        return 0;
    }
    return value;
}

auto TimelineFence::Wait(uint64_t value, uint32_t timeoutMs) const noexcept -> std::expected<void, ErrorCode> {
    if (_fence.Get()->GetCompletedValue() >= value) {
        return {};
    }
#if defined(_WIN32)
    if (const HRESULT hr = _fence.Get()->SetEventOnCompletion(value, static_cast<HANDLE>(_event)); FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    if (WaitForSingleObject(static_cast<HANDLE>(_event), timeoutMs) != WAIT_OBJECT_0) {
        return std::unexpected(ErrorCode {D3D12Error::FenceWaitFailed});
    }
    return {};
#else
    // Non-Windows hosts never run this backend; the wait degenerates to a
    // completed-value poll so the TU stays buildable everywhere.
    while (_fence.Get()->GetCompletedValue() < value) {
    }
    return {};
#endif
}

void TimelineFence::WaitOnQueue(ID3D12CommandQueue* queue, uint64_t value) const noexcept {
    queue->Wait(_fence.Get(), value);
}

auto TimelineFence::CompletedValue() const noexcept -> uint64_t {
    return _fence.Get()->GetCompletedValue();
}

} // namespace ZHLN::D3D12
