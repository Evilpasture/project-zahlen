#pragma once

// Device bring-up for the D3D12 backend -- the counterpart of
// Vk::Context's physical-device selection + logical device creation.
// Create() does the whole gate sequence in order:
//   1. Debug layer (when requested) + CreateDXGIFactory2.
//   2. Adapter enumeration and scoring: software adapters are skipped,
//      discrete GPUs outrank integrated (dedicated VRAM breaks ties) --
//      the same policy shape as Vk::SelectPhysicalDevice.
//   3. ID3D12Device creation, then the two hard feature gates this
//      backend is designed around: Enhanced Barriers (Options12) and
//      Shader Model 6.6. A device that fails either is rejected, not
//      degraded -- the barrier and bindless models have no fallback.
//   4. Direct + compute command queues.
// Windows-only in practice (DXGI); the TU is only built on WIN32.

#include "Com.hpp"
#include "D3D12Error.hpp"

#include <cstdint>
#include <expected>
#include <string>

#include <dxgi1_6.h>

namespace ZHLN::D3D12 {

struct AdapterInfo {
    std::string name;
    uint64_t    dedicatedVideoMemory = 0;
    bool        software             = false;
};

struct FeatureReport {
    bool     enhancedBarriers   = false;
    uint32_t highestShaderModel = 0; // D3D_SHADER_MODEL value
    bool     shaderModel66      = false;
};

class Context {
  public:
    Context() noexcept = default;

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;
    Context(Context&&) noexcept                = default;
    auto operator=(Context&&) noexcept -> Context& = default;

    // enableDebugLayer: pass the engine's dev-mode flag (ZHLN::isDev);
    // enables the D3D12 debug layer and DXGI factory debugging.
    [[nodiscard]] static auto Create(bool enableDebugLayer) noexcept -> std::expected<Context, ErrorCode>;

    [[nodiscard]] auto Device() const noexcept -> ID3D12Device* {
        return _device.Get();
    }
    // May be null on older drivers; feature-gated code should prefer the
    // newest interface it actually needs and check.
    [[nodiscard]] auto Device10() const noexcept -> ID3D12Device10* {
        return _device10.Get();
    }
    [[nodiscard]] auto Factory() const noexcept -> IDXGIFactory2* {
        return _factory.Get();
    }
    [[nodiscard]] auto GraphicsQueue() const noexcept -> ID3D12CommandQueue* {
        return _graphicsQueue.Get();
    }
    [[nodiscard]] auto ComputeQueue() const noexcept -> ID3D12CommandQueue* {
        return _computeQueue.Get();
    }
    [[nodiscard]] auto Adapter() const noexcept -> const AdapterInfo& {
        return _adapter;
    }
    [[nodiscard]] auto Features() const noexcept -> const FeatureReport& {
        return _features;
    }
    explicit operator bool() const noexcept {
        return _device != nullptr;
    }

  private:
    ComPtr<IDXGIFactory2>      _factory;
    ComPtr<ID3D12Device>       _device;
    ComPtr<ID3D12Device10>     _device10;
    ComPtr<ID3D12CommandQueue> _graphicsQueue;
    ComPtr<ID3D12CommandQueue> _computeQueue;
    AdapterInfo                _adapter;
    FeatureReport              _features;
};

} // namespace ZHLN::D3D12
