// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

#include "Instance.hpp"

namespace ZHLN::Vk {

// Vulkan instance/device bring-up failures for the Context subsystem.
enum class ContextError : uint8_t {
    InstanceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan instance creation failed">{}) = 1,
    NoSuitableDeviceFound ZHLN_ANNOTATION(ZHLN::Description<"No suitable Vulkan device found">{}),
    DeviceCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan logical device creation failed">{}),
};

class Context {
  public:
    class Builder;

    Context() noexcept = default;
    ~Context() noexcept;

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;

    Context(Context&& other) noexcept;
    auto operator=(Context&& other) noexcept -> Context&;

    [[nodiscard]] auto Instance() const noexcept -> VkInstance {
        return _instanceObject.Handle();
    }
    [[nodiscard]] auto Surface() const noexcept -> VkSurfaceKHR {
        return _surface;
    }
    [[nodiscard]] auto Device() const noexcept -> VkDevice {
        return _device.handle;
    }
    [[nodiscard]] auto GraphicsQueue() const noexcept -> VkQueue {
        return _device.graphics_queue;
    }
    [[nodiscard]] auto PresentQueue() const noexcept -> VkQueue {
        return _device.present_queue;
    }
    [[nodiscard]] auto TransferQueue() const noexcept -> VkQueue {
        return _device.transfer_queue;
    }
    [[nodiscard]] auto ComputeQueue() const noexcept -> VkQueue {
        return _device.compute_queue;
    }
    [[nodiscard]] auto Physical() const noexcept -> VkPhysicalDevice {
        return _physical.handle;
    }
    [[nodiscard]] auto PhysicalInfo() const noexcept -> const ZHLN_PhysicalDeviceInfo& {
        return _physical;
    }

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ZHLN_GetBufferDeviceAddress(_device.handle, buffer);
    }

    // ============================================================================
    // VK_EXT_descriptor_heap Entry Point Forwarding
    // ============================================================================

    [[nodiscard]] auto DescriptorHeapsSupported() const noexcept -> bool {
        return _device.descriptor_heap_enabled;
    }

    /// True when a resource descriptor may use a NULL payload. The renderer
    /// uses this for passes whose optional TLAS/BLAS is not available.
    [[nodiscard]] auto NullDescriptorSupported() const noexcept -> bool {
        return _device.null_descriptor_enabled;
    }

    void CmdBindResourceHeap(VkCommandBuffer cmd, const VkBindHeapInfoEXT* info) const noexcept {
        if (_device.pfn_cmd_bind_resource_heap != nullptr) {
            _device.pfn_cmd_bind_resource_heap(cmd, info);
        }
    }

    void CmdBindSamplerHeap(VkCommandBuffer cmd, const VkBindHeapInfoEXT* info) const noexcept {
        if (_device.pfn_cmd_bind_sampler_heap != nullptr) {
            _device.pfn_cmd_bind_sampler_heap(cmd, info);
        }
    }

    void CmdPushData(VkCommandBuffer cmd, const VkPushDataInfoEXT* info) const noexcept {
        if (_device.pfn_cmd_push_data != nullptr) {
            _device.pfn_cmd_push_data(cmd, info);
        }
    }

    [[nodiscard]] auto
        WriteResourceDescriptors(uint32_t count, const VkResourceDescriptorInfoEXT* resources, const VkHostAddressRangeEXT* descriptors) const noexcept
        -> VkResult {
        if (_device.pfn_write_resource_descriptors == nullptr) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        return _device.pfn_write_resource_descriptors(_device.handle, count, resources, descriptors);
    }

    [[nodiscard]] auto
        WriteSamplerDescriptors(uint32_t count, const VkSamplerCreateInfo* samplers, const VkHostAddressRangeEXT* descriptors) const noexcept -> VkResult {
        if (_device.pfn_write_sampler_descriptors == nullptr) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        return _device.pfn_write_sampler_descriptors(_device.handle, count, samplers, descriptors);
    }

    // ============================================================================
    // VK_EXT_mesh_shader Entry Point Forwarding
    // ============================================================================

    /// True only when the extension, its entry points AND the required hardware
    /// limits are all present (see ZHLN_MeshShaderLimitsSufficient).
    [[nodiscard]] auto MeshShadersSupported() const noexcept -> bool {
        return _device.mesh_shader_enabled;
    }

    void CmdDrawMeshTasks(VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const noexcept {
        ZHLN_CmdDrawMeshTasks(&_device, cmd, groupCountX, groupCountY, groupCountZ);
    }

    void CmdDrawMeshTasksIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) const noexcept {
        ZHLN_CmdDrawMeshTasksIndirect(&_device, cmd, buffer, offset, drawCount, stride);
    }

    void CmdDrawMeshTasksIndirectCount(
        VkCommandBuffer cmd,
        VkBuffer        buffer,
        VkDeviceSize    offset,
        VkBuffer        countBuffer,
        VkDeviceSize    countBufferOffset,
        uint32_t        maxDrawCount,
        uint32_t        stride
    ) const noexcept {
        ZHLN_CmdDrawMeshTasksIndirectCount(&_device, cmd, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    [[nodiscard]] auto MeshShaderLimits() const noexcept -> ZHLN_MeshShaderLimits {
        return ZHLN_QueryMeshShaderLimits(_physical.handle);
    }

    [[nodiscard("Always verify context initialization; check Valid() before use")]]
    auto Valid() const noexcept -> bool {
        return _device.handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

  private:
    // Qualified: the Instance() accessor above shadows the class name in
    // class scope. Owns the handle, the persistent debug messenger, and the
    // validation/device-lost diagnostics.
    Vk::Instance             _instanceObject {};
    VkSurfaceKHR             _surface        = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo  _physical       = {};
    ZHLN_Device              _device         = {};
};

using ValidationMode = ZHLN_ValidationMode;

class Context::Builder {
  public:
    constexpr Builder() noexcept = default;

    constexpr Builder& AppName(std::string_view name) noexcept {
        _appName = name;
        return *this;
    }

    constexpr Builder& AppVersion(uint32_t version) noexcept {
        _appVersion = version;
        return *this;
    }

    constexpr Builder& ValidationMode(ValidationMode mode) noexcept {
        _validationMode = mode;
        return *this;
    }

    constexpr Builder& Instance(VkInstance inst) noexcept {
        _instanceView = inst;
        return *this;
    }

    // Owning variant: transfers a Vk::Instance (created via BuildInstance())
    // into this builder so Build() can move it into the Context. Required for
    // the final Build(); the raw setter above only provides a non-owning view
    // for query paths (SelectPhysicalDevice and friends).
    constexpr Builder& Instance(Vk::Instance&& inst) noexcept {
        _instanceObject = std::move(inst);
        _instanceView   = _instanceObject.Handle();
        return *this;
    }

    constexpr Builder& Surface(VkSurfaceKHR surf) noexcept {
        _surface = surf;
        return *this;
    }

    constexpr Builder& PhysicalDevice(const ZHLN_PhysicalDeviceInfo& physical) noexcept {
        _physical = physical;
        return *this;
    }

    constexpr Builder& InstanceExtensions(std::span<const std::string_view> exts) noexcept {
        _instanceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    // Support standard spans
    constexpr Builder& DeviceExtensions(std::span<const char* const> exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    // Direct overload to resolve single-step implicit conversions from ExtensionResult
    constexpr Builder& DeviceExtensions(const std::vector<const char*>& exts) noexcept {
        _deviceExtensions.assign(exts.begin(), exts.end());
        return *this;
    }

    constexpr Builder& DeviceFeatures(const VkPhysicalDeviceFeatures2* features) noexcept {
        _features = features;
        return *this;
    }

    constexpr Builder& ScoreFunction(ZHLN_DeviceScoreFn scoreFn, void* userdata = nullptr) noexcept {
        _scoreFn       = scoreFn;
        _scoreUserdata = userdata;
        return *this;
    }

    // --- Build Steps ---
    // Creates the instance and moves OWNERSHIP out: hold the returned Vk::Instance
    // and feed it back via Instance(Vk::Instance&&) before Build(). (A temporary
    // Builder that owns the instance destroys it when it goes out of scope.)
    [[nodiscard]] std::expected<Vk::Instance, ZHLN::Error>            BuildInstance() noexcept;
    [[nodiscard]] std::expected<ZHLN_PhysicalDeviceInfo, ZHLN::Error> SelectPhysicalDevice() const noexcept;
    [[nodiscard]] std::expected<Context, ZHLN::Error>                 Build() noexcept;

  private:
    std::string_view   _appName        = "ZHLN Engine";
    uint32_t           _appVersion     = VK_MAKE_API_VERSION(0, 1, 0, 0);
    Vk::ValidationMode _validationMode = ZHLN_VALIDATION_ON;

    Vk::Instance            _instanceObject {};
    VkInstance              _instanceView  = VK_NULL_HANDLE;
    VkSurfaceKHR            _surface       = VK_NULL_HANDLE;
    ZHLN_PhysicalDeviceInfo _physical      = {};

    std::vector<std::string_view>    _instanceExtensions;
    std::vector<const char*>         _deviceExtensions;
    const VkPhysicalDeviceFeatures2* _features      = nullptr;
    ZHLN_DeviceScoreFn               _scoreFn       = nullptr;
    void*                            _scoreUserdata = nullptr;
};

} // namespace ZHLN::Vk
