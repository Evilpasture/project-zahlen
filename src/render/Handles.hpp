#pragma once

namespace ZHLN::Vk {

// ============================================================================
// RAII Handles
// ============================================================================

template <typename T, auto DeleterFn>
class DeviceHandle {
  public:
    DeviceHandle() noexcept = default;
    DeviceHandle(const VkDevice device, const T raw) noexcept;
    ~DeviceHandle() noexcept;

    DeviceHandle(const DeviceHandle&)                    = delete;
    auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

    DeviceHandle(DeviceHandle&& other) noexcept;
    auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle&;

    [[nodiscard]] constexpr auto Get() const noexcept -> T;
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool;
    constexpr explicit           operator bool() const noexcept;
    [[nodiscard]] constexpr auto Release() noexcept -> T;

  private:
    VkDevice _device = VK_NULL_HANDLE;
    T        _raw    = VK_NULL_HANDLE;
};

// NOTE: RAII handles which call destructors from C. Inlinable with LTO.
using ShaderModule        = DeviceHandle<VkShaderModule, ZHLN_DestroyShaderModule>;
using PipelineLayout      = DeviceHandle<VkPipelineLayout, ZHLN_DestroyPipelineLayout>;
using Pipeline            = DeviceHandle<VkPipeline, ZHLN_DestroyPipeline>;
using Semaphore           = DeviceHandle<VkSemaphore, ZHLN_DestroySemaphore>;
using Sampler             = DeviceHandle<VkSampler, ZHLN_DestroySampler>;
using DescriptorSetLayout = DeviceHandle<VkDescriptorSetLayout, ZHLN_DestroyDescriptorSetLayout>;
using DescriptorPool      = DeviceHandle<VkDescriptorPool, ZHLN_DestroyDescriptorPool>;
using ImageView           = DeviceHandle<VkImageView, ZHLN_DestroyImageView>;

} // namespace ZHLN::Vk
#include "Handles.inl"
