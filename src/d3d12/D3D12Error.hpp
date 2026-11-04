#pragma once

// Error vocabulary for the d3d12 backend, mirroring src/vulkan/VkError.hpp:
// raw HRESULT failures convert through ToError(HRESULT) keeping the code
// in the ErrorCode's value word, while failures that are not HRESULTs
// (feature gates, capacity) use the annotated D3D12Error enum. Start all
// enumerators at 1 -- ErrorCode reserves 0 for "no error".

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Hash.hpp>

#include <cstdint>
#include <string_view>

namespace ZHLN::D3D12 {

enum class D3D12Error : uint8_t {
    NoSuitableAdapter ZHLN_ANNOTATION(ZHLN::Description<"No non-software DXGI adapter was found"> {}) = 1,
    EnhancedBarriersUnsupported ZHLN_ANNOTATION(ZHLN::Description<"D3D12_FEATURE_D3D12_OPTIONS12::EnhancedBarriersSupported is false; the backend requires enhanced barriers"> {}),
    ShaderModelTooLow ZHLN_ANNOTATION(ZHLN::Description<"Highest shader model is below SM 6.6; the backend requires SM 6.6 bindless heaps"> {}),
    DescriptorSlotsExhausted ZHLN_ANNOTATION(ZHLN::Description<"A global descriptor heap ran out of slots"> {}),
    FenceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"ID3D12Device::CreateFence or its completion event failed"> {}),
    FenceWaitFailed ZHLN_ANNOTATION(ZHLN::Description<"Waiting on a timeline fence's completion event failed or timed out"> {}),
    SwapchainCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"IDXGISwapChain4 could not be obtained for the flip-model swapchain"> {}),
};

inline constexpr uint32_t      kHresultCategoryHash = Hash32("ZHLN::D3D12::HResult");
inline constexpr ErrorCategory kHresultCategory     = {
    .name      = "D3D12 HRESULT",
    .to_string = [](const uint32_t v) noexcept -> std::string_view {
        switch (static_cast<int32_t>(v)) {
            case static_cast<int32_t>(0x887A0005): return "DXGI_ERROR_DEVICE_REMOVED";
            case static_cast<int32_t>(0x887A0006): return "DXGI_ERROR_DEVICE_HUNG";
            case static_cast<int32_t>(0x887A0007): return "DXGI_ERROR_DEVICE_RESET";
            case static_cast<int32_t>(0x80070057): return "E_INVALIDARG";
            case static_cast<int32_t>(0x8007000E): return "E_OUTOFMEMORY";
            default: return "HRESULT failure";
        }
    },
    .to_name = [](const uint32_t) noexcept -> std::string_view { return "HRESULT"; },
};

// Convert a FAILED HRESULT. Never call with S_OK (ErrorCode value 0 is
// "no error"); call sites check SUCCEEDED(hr) first, exactly like
// Vk::ToError is only handed failures.
[[nodiscard]] constexpr auto ToError(int32_t hresult) noexcept -> ErrorCode {
    if !consteval {
        RegisterErrorCategory(kHresultCategoryHash, &kHresultCategory);
    }
    return ErrorCode {kHresultCategoryHash, static_cast<uint32_t>(hresult)};
}

} // namespace ZHLN::D3D12
