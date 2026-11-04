// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"

#include <array>

namespace ZHLN::D3D12 {
namespace {

// DXGI's WCHAR description narrowed lossily; adapter names are diagnostic
// only, so non-ASCII collapses to '?' rather than failing bring-up.
[[nodiscard]] auto NarrowName(const WCHAR* wide) -> std::string {
    std::string out;
    for (const WCHAR* p = wide; *p != L'\0'; ++p) {
        out.push_back(*p >= 0x20 && *p < 0x7F ? static_cast<char>(*p) : '?');
    }
    return out;
}

[[nodiscard]] auto AdapterScore(const DXGI_ADAPTER_DESC1& desc) noexcept -> uint64_t {
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        return 0; // WARP / Basic Render: never selected while hardware exists
    }
    // Discrete-class adapters (dedicated VRAM present) outrank integrated
    // parts regardless of size; within a class, more dedicated memory wins.
    const uint64_t discreteBonus = desc.DedicatedVideoMemory > 0 ? (uint64_t {1} << 62) : 0;
    return discreteBonus | (desc.DedicatedVideoMemory >> 10);
}

} // namespace

auto Context::Create(bool enableDebugLayer) noexcept -> std::expected<Context, ErrorCode> {
    Context ctx;

    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(debug.AddressOf())))) {
            debug->EnableDebugLayer();
        }
    }

    const UINT factoryFlags = enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (const HRESULT hr = CreateDXGIFactory2(factoryFlags, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(ctx._factory.AddressOf()));
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }

    // Adapter pick: prefer EnumAdapterByGpuPreference (Factory6), fall
    // back to plain enumeration on older factories.
    ComPtr<IDXGIAdapter1> bestAdapter;
    uint64_t              bestScore = 0;
    const auto            consider  = [&](IDXGIAdapter1* adapter) {
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            return;
        }
        const uint64_t score = AdapterScore(desc);
        if (score > bestScore) {
            bestScore   = score;
            bestAdapter = ComPtr<IDXGIAdapter1>(adapter);
            adapter->AddRef(); // `consider` borrows; the ComPtr takes its own reference
        }
    };
    if (auto factory6 = ctx._factory.As<IDXGIFactory6>(); factory6) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                             reinterpret_cast<void**>(adapter.AddressOf())))) {
                break;
            }
            consider(adapter.Get());
        }
    } else {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(ctx._factory->EnumAdapters1(i, adapter.AddressOf()))) {
                break;
            }
            consider(adapter.Get());
        }
    }
    if (!bestAdapter) {
        return std::unexpected(ErrorCode {D3D12Error::NoSuitableAdapter});
    }

    {
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(bestAdapter->GetDesc1(&desc))) {
            ctx._adapter = AdapterInfo {
                .name                 = NarrowName(desc.Description),
                .dedicatedVideoMemory = desc.DedicatedVideoMemory,
                .software             = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
            };
        }
    }

    if (const HRESULT hr = D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                             reinterpret_cast<void**>(ctx._device.AddressOf()));
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    ctx._device10 = ctx._device.As<ID3D12Device10>();

    // Hard gates: no enhanced barriers or no SM 6.6 means this backend
    // cannot express its barrier/bindless model; fail creation outright.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options {};
        if (FAILED(ctx._device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)))) {
            return std::unexpected(ErrorCode {D3D12Error::EnhancedBarriersUnsupported});
        }
        ctx._features.enhancedBarriers = options.EnhancedBarriersSupported != FALSE;
        if (!ctx._features.enhancedBarriers) {
            return std::unexpected(ErrorCode {D3D12Error::EnhancedBarriersUnsupported});
        }
    }
    {
        D3D12_FEATURE_DATA_SHADER_MODEL model {.HighestShaderModel = D3D_SHADER_MODEL_6_6};
        if (FAILED(ctx._device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))) {
            return std::unexpected(ErrorCode {D3D12Error::ShaderModelTooLow});
        }
        ctx._features.highestShaderModel = static_cast<uint32_t>(model.HighestShaderModel);
        ctx._features.shaderModel66      = model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
        if (!ctx._features.shaderModel66) {
            return std::unexpected(ErrorCode {D3D12Error::ShaderModelTooLow});
        }
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc {
        .Type     = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    if (const HRESULT hr = ctx._device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                                           reinterpret_cast<void**>(ctx._graphicsQueue.AddressOf()));
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }
    D3D12_COMMAND_QUEUE_DESC computeDesc = queueDesc;
    computeDesc.Type                     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (const HRESULT hr = ctx._device->CreateCommandQueue(&computeDesc, __uuidof(ID3D12CommandQueue),
                                                           reinterpret_cast<void**>(ctx._computeQueue.AddressOf()));
        FAILED(hr)) {
        return std::unexpected(ToError(hr));
    }

    return ctx;
}

} // namespace ZHLN::D3D12
