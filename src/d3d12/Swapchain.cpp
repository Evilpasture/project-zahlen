// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Swapchain.hpp"

#include "RenderTargets.hpp" // anchor: keep the header compiling in a shipped TU until the pass context consumes it

namespace ZHLN::D3D12 {

auto Swapchain::Create(
    ID3D12Device* device, IDXGIFactory2* factory, ID3D12CommandQueue* queue, void* windowHandle, uint32_t width, uint32_t height,
    uint32_t bufferCount
) noexcept -> std::expected<Swapchain, ErrorCode> {
    Swapchain swapchain;
    swapchain._bufferCount = bufferCount;
    swapchain._format      = DXGI_FORMAT_B8G8R8A8_UNORM;

    auto rtvHeap = DescriptorHeap::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, bufferCount, false);
    if (!rtvHeap) {
        return std::unexpected(rtvHeap.error());
    }
    swapchain._rtvHeap = std::move(*rtvHeap);
    for (uint32_t i = 0; i < bufferCount; ++i) {
        // The RTV heap is private to this swapchain and exactly sized, so
        // allocation cannot fail after the heap exists.
        swapchain._rtvSlots[i] = *swapchain._rtvHeap.Allocate();
    }

    const DXGI_SWAP_CHAIN_DESC1 desc {
        .Width       = width,
        .Height      = height,
        .Format      = swapchain._format,
        .Stereo      = FALSE,
        .SampleDesc  = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = bufferCount,
        .Scaling     = DXGI_SCALING_NONE,
        .SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode   = DXGI_ALPHA_MODE_IGNORE,
        .Flags       = 0,
    };
    ComPtr<IDXGISwapChain1> base;
    if (const HRESULT hr = factory->CreateSwapChainForHwnd(
            queue, static_cast<HWND>(windowHandle), &desc, nullptr, nullptr, base.AddressOf()
        );
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    swapchain._swapchain = base.As<IDXGISwapChain4>();
    if (!swapchain._swapchain) {
        return std::unexpected(ErrorCode {D3D12Error::SwapchainCreationFailed});
    }

    if (auto views = swapchain.CreateViews(device); !views) {
        return std::unexpected(views.error());
    }
    return swapchain;
}

auto Swapchain::CreateViews(ID3D12Device* device) noexcept -> std::expected<void, ErrorCode> {
    for (uint32_t i = 0; i < _bufferCount; ++i) {
        if (const HRESULT hr = _swapchain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(_backBuffers[i].AddressOf()));
            FAILED(hr)) {
            return std::unexpected(ToError(hr));
        }
        device->CreateRenderTargetView(_backBuffers[i].Get(), nullptr, RtvHandle(i));
    }
    return {};
}

void Swapchain::ReleaseBuffers() noexcept {
    for (auto& buffer: _backBuffers) {
        buffer.Reset();
    }
}

auto Swapchain::CurrentBackBufferIndex() const noexcept -> uint32_t {
    return _swapchain->GetCurrentBackBufferIndex();
}

auto Swapchain::Present(uint32_t syncInterval) noexcept -> std::expected<void, ErrorCode> {
    if (const HRESULT hr = _swapchain->Present(syncInterval, 0); FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    return {};
}

auto Swapchain::Resize(ID3D12Device* device, uint32_t width, uint32_t height) noexcept -> std::expected<void, ErrorCode> {
    // All references to the back buffers must be released before
    // ResizeBuffers; in-flight GPU work is the caller's fence problem.
    ReleaseBuffers();
    if (const HRESULT hr = _swapchain->ResizeBuffers(_bufferCount, width, height, DXGI_FORMAT_UNKNOWN, 0); FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    return CreateViews(device);
}

} // namespace ZHLN::D3D12
