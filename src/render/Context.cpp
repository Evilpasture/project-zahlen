// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"

namespace ZHLN::Vk {

VkInstance CreateInstance(std::string_view appName, uint32_t appVersion, std::span<const std::string_view> extensions, bool enableValidation) noexcept {
    std::vector<const char*> c_strings;
    c_strings.reserve(extensions.size());
    for (const auto& sv: extensions) {
        c_strings.push_back(sv.data());
    }

    ZHLN_InstanceDesc inst_desc = {
        .app_name          = {},
        .version           = appVersion,
        .extension_count   = static_cast<uint32_t>(c_strings.size()),
        .severity_flags    = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .extensions        = c_strings.data(),
        .enable_validation = enableValidation
    };

    const size_t copy_size = ZHLN::Min(appName.size(), sizeof(inst_desc.app_name) - 1);
    std::memcpy(inst_desc.app_name, appName.data(), copy_size);
    inst_desc.app_name[copy_size] = '\0';

    return ZHLN_CreateInstance(&inst_desc);
}

ZHLN_PhysicalDeviceInfo SelectDevice(VkInstance instance, VkSurfaceKHR surface) noexcept {
    ZHLN_DeviceSelectDesc select_desc = {.instance = instance, .surface = surface, .score_fn = nullptr, .score_userdata = nullptr};
    return ZHLN_SelectPhysicalDevice(&select_desc);
}

// ============================================================================
// Context Implementation
// ============================================================================

Context::~Context() noexcept {
    if (_device.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(_device.handle, nullptr);
    }
    if (_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(_instance, nullptr);
    }
}

Context::Context(Context&& other) noexcept:
    _instance(std::exchange(other._instance, VK_NULL_HANDLE)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
    _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        if (_device.handle != VK_NULL_HANDLE) {
            vkDestroyDevice(_device.handle, nullptr);
        }
        if (_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(_instance, nullptr);
        }

        _instance = std::exchange(other._instance, VK_NULL_HANDLE);
        _surface  = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical = other._physical;
        _device   = other._device;

        other._physical = {};
        other._device   = {};
    }
    return *this;
}

auto Context::Create(const ZHLN_InstanceDesc& instanceDesc, const ZHLN_DeviceSelectDesc& selectDesc, const ZHLN_DeviceDesc& deviceDesc) noexcept -> Context {
    Context ctx;

    ctx._instance = ZHLN_CreateInstance(&instanceDesc);
    if (ctx._instance == VK_NULL_HANDLE) {
        return {};
    }

    ctx._surface = selectDesc.surface;

    const ZHLN_DeviceSelectDesc safe_select = {
        .instance       = ctx._instance,
        .surface        = selectDesc.surface,
        .score_fn       = selectDesc.score_fn,
        .score_userdata = selectDesc.score_userdata,
    };
    ctx._physical = ZHLN_SelectPhysicalDevice(&safe_select);

    if (ctx._physical.handle == VK_NULL_HANDLE) {
        return {};
    }

    const ZHLN_DeviceDesc safe_device = {
        .physical          = &ctx._physical,
        .extensions        = deviceDesc.extensions,
        .extension_count   = deviceDesc.extension_count,
        .features          = deviceDesc.features,
        .enable_validation = deviceDesc.enable_validation,
    };
    ctx._device = ZHLN_CreateDevice(&safe_device);

    return ctx;
}

auto Context::Create(VkInstance instance, VkSurfaceKHR surface, const ZHLN_PhysicalDeviceInfo& physical, const ZHLN_DeviceDesc& deviceDesc) noexcept
    -> Context {
    Context ctx;
    ctx._instance = instance;
    ctx._surface  = surface;
    ctx._physical = physical;

    const ZHLN_DeviceDesc safe_device = {
        .physical          = &ctx._physical,
        .extensions        = deviceDesc.extensions,
        .extension_count   = deviceDesc.extension_count,
        .features          = deviceDesc.features,
        .enable_validation = deviceDesc.enable_validation,
    };
    ctx._device = ZHLN_CreateDevice(&safe_device);

    return ctx;
}

} // namespace ZHLN::Vk
