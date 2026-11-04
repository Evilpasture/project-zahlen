#pragma once

// DXGI flip-model presentation -- the D3D12 counterpart of the Vulkan
// swapchain presenter. FLIP_DISCARD back buffers are acquired by index
// (GetCurrentBackBufferIndex, the analogue of vkAcquireNextImageKHR) and
// presented with Present(1, 0). The presenter owns its RTV heap (one
// slot per back buffer, allocated through the same DescriptorHeap class
// as the global heaps) and recreates views on Resize. Layout transitions
// of the back buffer (PRESENT <-> RENDER_TARGET) are the caller's job
// via Barriers.hpp, exactly as swapchain image transitions are explicit
// on the Vulkan side.

#include "Com.hpp"
#include "Context.hpp"
#include "DescriptorHeap.hpp"

#include <array>
#include <cstdint>
#include <expected>

namespace ZHLN::D3D12 {

class Swapchain {
  public:
    static constexpr uint32_t kMaxBackBuffers = 4;

    Swapchain() noexcept = default;

    Swapchain(const Swapchain&)                    = delete;
    auto operator=(const Swapchain&) -> Swapchain& = delete;
    Swapchain(Swapchain&&) noexcept                = default;
    auto operator=(Swapchain&&) noexcept -> Swapchain& = default;

    // windowHandle is an HWND, taken as void* so this header does not
    // pull <windows.h> into includers.
    [[nodiscard]] static auto Create(
        ID3D12Device* device, IDXGIFactory2* factory, ID3D12CommandQueue* queue, void* windowHandle, uint32_t width, uint32_t height,
        uint32_t bufferCount = 3
    ) noexcept -> std::expected<Swapchain, ErrorCode>;

    [[nodiscard]] auto CurrentBackBufferIndex() const noexcept -> uint32_t;
    [[nodiscard]] auto BackBuffer(uint32_t index) const noexcept -> ID3D12Resource* {
        return _backBuffers[index].Get();
    }
    [[nodiscard]] auto CurrentBackBuffer() const noexcept -> ID3D12Resource* {
        return BackBuffer(CurrentBackBufferIndex());
    }
    [[nodiscard]] auto RtvHandle(uint32_t index) const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE {
        return _rtvHeap.CpuHandle(_rtvSlots[index]);
    }
    [[nodiscard]] auto CurrentRtvHandle() const noexcept -> D3D12_CPU_DESCRIPTOR_HANDLE {
        return RtvHandle(CurrentBackBufferIndex());
    }
    [[nodiscard]] auto BufferCount() const noexcept -> uint32_t {
        return _bufferCount;
    }
    [[nodiscard]] auto Format() const noexcept -> DXGI_FORMAT {
        return _format;
    }

    [[nodiscard]] auto Present(uint32_t syncInterval = 1) noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto Resize(ID3D12Device* device, uint32_t width, uint32_t height) noexcept -> std::expected<void, ErrorCode>;

    explicit operator bool() const noexcept {
        return _swapchain != nullptr;
    }

  private:
    [[nodiscard]] auto CreateViews(ID3D12Device* device) noexcept -> std::expected<void, ErrorCode>;
    void               ReleaseBuffers() noexcept;

    ComPtr<IDXGISwapChain4>                          _swapchain;
    DescriptorHeap                                   _rtvHeap;
    std::array<ComPtr<ID3D12Resource>, kMaxBackBuffers> _backBuffers;
    std::array<uint32_t, kMaxBackBuffers>               _rtvSlots {};
    uint32_t                                            _bufferCount = 0;
    DXGI_FORMAT                                         _format      = DXGI_FORMAT_B8G8R8A8_UNORM;
};

} // namespace ZHLN::D3D12
