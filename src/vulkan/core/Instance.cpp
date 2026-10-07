// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Instance.hpp"
#include "Extensions.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

std::atomic<Instance*>       Instance::_active {nullptr};
std::atomic<DiagnosticsSink> Instance::_registeredSink {DiagnosticsSink {}};

namespace {

[[nodiscard]] auto EnumerateInstanceLayers() noexcept -> std::vector<VkLayerProperties> {
    std::vector<VkLayerProperties> layers;
    for (;;) {
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        layers.resize(count);
        const VkResult result = vkEnumerateInstanceLayerProperties(&count, layers.data());
        if (result == VK_SUCCESS) {
            layers.resize(count);
            return layers;
        }
        if (result != VK_INCOMPLETE) {
            return {};
        }
    }
}

[[nodiscard]] auto HasLayer(const std::span<const VkLayerProperties> layers, const std::string_view name) noexcept -> bool {
    return std::ranges::any_of(layers, [name](const VkLayerProperties& layer) { return name == layer.layerName; });
}

[[nodiscard]] auto ContainsAny(const std::string_view text, const std::span<const std::string_view> needles) noexcept -> bool {
    return std::ranges::any_of(needles, [text](const std::string_view needle) { return text.find(needle) != std::string_view::npos; });
}

} // namespace

Instance::Instance() noexcept: _debugState(new (std::nothrow) DebugState {}) {
    RebindDebugState();
}

void Instance::UseDiagnostics(const DiagnosticsSink sink) noexcept {
    _registeredSink.store(sink.Valid() ? sink : DiagnosticsSink {}, std::memory_order::release);
}

void Instance::RebindDebugState() noexcept {
    if (_debugState != nullptr) {
        _debugState->owner = this;
    }
}

void Instance::Destroy() noexcept {
    if (_handle != VK_NULL_HANDLE) {
        if (_addressBindingMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(_handle, _addressBindingMessenger, nullptr);
        }
        if (_messenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(_handle, _messenger, nullptr);
        }
        vkDestroyInstance(_handle, nullptr);
        _handle                  = VK_NULL_HANDLE;
        _messenger               = VK_NULL_HANDLE;
        _addressBindingMessenger = VK_NULL_HANDLE;
    }

    Instance* expected = this;
    _active.compare_exchange_strong(expected, nullptr, std::memory_order::release, std::memory_order::relaxed);
}

Instance::~Instance() noexcept {
    Destroy();
}

Instance::Instance(Instance&& other) noexcept:
    _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
    _messenger(std::exchange(other._messenger, VK_NULL_HANDLE)),
    _addressBindingMessenger(std::exchange(other._addressBindingMessenger, VK_NULL_HANDLE)),
    _debugState(std::move(other._debugState)),
    _validationErrors(other._validationErrors.load(std::memory_order::relaxed)),
    _deviceLost(other._deviceLost.load(std::memory_order::relaxed)),
    _validationTarget(other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget),
    _deviceLostTarget(other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget) {
    RebindDebugState();
    if (_active.load(std::memory_order::acquire) == &other) {
        _active.store(this, std::memory_order::release);
    }
    other._validationErrors.store(0, std::memory_order::relaxed);
    other._deviceLost.store(0, std::memory_order::relaxed);
    other._validationTarget = &other._validationErrors;
    other._deviceLostTarget = &other._deviceLost;
}

auto Instance::operator=(Instance&& other) noexcept -> Instance& {
    if (this != &other) {
        Destroy();

        _handle                  = std::exchange(other._handle, VK_NULL_HANDLE);
        _messenger               = std::exchange(other._messenger, VK_NULL_HANDLE);
        _addressBindingMessenger = std::exchange(other._addressBindingMessenger, VK_NULL_HANDLE);
        _debugState              = std::move(other._debugState);
        _validationErrors        = other._validationErrors.load(std::memory_order::relaxed);
        _deviceLost              = other._deviceLost.load(std::memory_order::relaxed);
        _validationTarget = other._validationTarget == &other._validationErrors ? &_validationErrors : other._validationTarget;
        _deviceLostTarget = other._deviceLostTarget == &other._deviceLost ? &_deviceLost : other._deviceLostTarget;

        RebindDebugState();
        if (_active.load(std::memory_order::acquire) == &other) {
            _active.store(this, std::memory_order::release);
        }
        other._validationErrors.store(0, std::memory_order::relaxed);
        other._deviceLost.store(0, std::memory_order::relaxed);
        other._validationTarget = &other._validationErrors;
        other._deviceLostTarget = &other._deviceLost;
    }
    return *this;
}

auto VKAPI_CALL Instance::DebugCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    const VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData
) noexcept -> VkBool32 {
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) != 0) {
        if (callbackData != nullptr) {
            for (auto* node = static_cast<const VkBaseInStructure*>(callbackData->pNext); node != nullptr; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
                    GPUAddressTracker::Get().OnBindingEvent(
                        *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(node), callbackData
                    );
                    break;
                }
            }
        }
        return VK_FALSE;
    }

    auto* const state = static_cast<DebugState*>(userData);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 && state != nullptr && state->owner != nullptr) {
        state->owner->_validationTarget->fetch_add(1, std::memory_order::relaxed);
    }

    const std::string_view message = callbackData != nullptr && callbackData->pMessage != nullptr
        ? std::string_view(callbackData->pMessage)
        : std::string_view("(no Vulkan diagnostic text)");

    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ZHLN::LogError("[Vulkan] {}", message);
        constexpr std::array<std::string_view, 4> kGpuBoundsMarkers = {
            "out of bounds", "Out of bounds", "OOB", "bounds check failed"
        };
        if (ContainsAny(message, kGpuBoundsMarkers)) {
            ZHLN::LogError("[Vulkan] GPU-assisted validation detected an out-of-bounds shader access; aborting.");
            std::abort();
        }
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        ZHLN::LogWarning("[Vulkan] {}", message);
    } else {
        ZHLN::Log("[Vulkan] {}", message);
    }

    return VK_FALSE;
}

auto Instance::Create(
    const std::string_view appName,
    const uint32_t appVersion,
    const std::span<const std::string_view> extensions,
    const ValidationMode validation
) noexcept -> Instance {
    Instance result;
    if (result._debugState == nullptr) {
        return result;
    }

    const DiagnosticsSink sink = _registeredSink.load(std::memory_order::acquire);
    if (sink.Valid()) {
        result._validationTarget = sink.validation;
        result._deviceLostTarget = sink.deviceLost;
    }

    if (volkInitialize() != VK_SUCCESS) {
        ZHLN::LogError("[Vulkan] No Vulkan loader is available; volkInitialize() failed.");
        return result;
    }

    const std::vector<VkExtensionProperties> availableExtensions = EnumerateInstanceExtensions();
    const std::vector<VkLayerProperties>     availableLayers     = EnumerateInstanceLayers();

    constexpr std::string_view kValidationLayer = "VK_LAYER_KHRONOS_validation";
    bool enableValidation = validation != ValidationMode::Off;
    bool gpuValidation    = validation == ValidationMode::GPU;
    if (enableValidation && !HasLayer(availableLayers, kValidationLayer)) {
        ZHLN::LogWarning(
            "[Vulkan] Validation layer {} is not available; continuing without validation. Install the Vulkan SDK or configure VK_LAYER_PATH to enable it.",
            kValidationLayer
        );
        enableValidation = false;
        gpuValidation    = false;
    }

    std::vector<std::string> enabledExtensionNames;
    enabledExtensionNames.reserve(extensions.size() + 3);
    const auto addIfSupported = [&](const std::string_view name, const std::string_view messagePrefix) {
        if (!HasExtension(availableExtensions, name)) {
            ZHLN::LogWarning("{}{}", messagePrefix, name);
            return false;
        }
        if (!std::ranges::contains(enabledExtensionNames, name)) {
            enabledExtensionNames.emplace_back(name);
        }
        return true;
    };

    for (const std::string_view extension: extensions) {
        addIfSupported(extension, "[Vulkan] Skipping unsupported instance extension: ");
    }

    const bool debugUtilsEnabled = validation != ValidationMode::Off &&
        addIfSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, "[Vulkan] Debug utils is unavailable: ");
    const bool validationFeaturesEnabled = enableValidation && gpuValidation &&
        addIfSupported(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, "[Vulkan] GPU-assisted validation is unavailable: ");
    if (gpuValidation && !validationFeaturesEnabled) {
        gpuValidation = false;
    }
    const bool layerSettingsEnabled = enableValidation && gpuValidation &&
        addIfSupported(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, "[Vulkan] Layer settings extension is unavailable: ");

    std::vector<const char*> enabledExtensions;
    enabledExtensions.reserve(enabledExtensionNames.size());
    for (const std::string& name: enabledExtensionNames) {
        enabledExtensions.push_back(name.c_str());
    }

    std::string appNameStorage(appName);
    const VkApplicationInfo applicationInfo {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = appNameStorage.c_str(),
        .applicationVersion = appVersion,
        .apiVersion         = VK_API_VERSION_1_3,
    };

    std::array<VkValidationFeatureEnableEXT, 2> enabledValidationFeatures {};
    uint32_t enabledValidationFeatureCount = 0;
    if (gpuValidation && validationFeaturesEnabled) {
        enabledValidationFeatures[enabledValidationFeatureCount++] = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
        enabledValidationFeatures[enabledValidationFeatureCount++] = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT;
    }

    VkValidationFeaturesEXT validationFeatures {
        .sType                         = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext                         = nullptr,
        .enabledValidationFeatureCount = enabledValidationFeatureCount,
        .pEnabledValidationFeatures    = enabledValidationFeatureCount != 0 ? enabledValidationFeatures.data() : nullptr,
        .disabledValidationFeatureCount = 0,
        .pDisabledValidationFeatures    = nullptr,
    };

    constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
    const VkBool32 forceRobustness = VK_TRUE;
    const VkBool32 dumpDescriptors = VK_TRUE;
    const VkBool32 dumpToStdout    = VK_TRUE;
    const std::array<VkLayerSettingEXT, 3> layerSettings = {{
        {.pLayerName = kValidationLayerName, .pSettingName = "gpuav_force_on_robustness", .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount = 1, .pValues = &forceRobustness},
        {.pLayerName = kValidationLayerName, .pSettingName = "gpu_dump_descriptors", .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount = 1, .pValues = &dumpDescriptors},
        {.pLayerName = kValidationLayerName, .pSettingName = "gpu_dump_to_stdout", .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         .valueCount = 1, .pValues = &dumpToStdout},
    }};

    VkLayerSettingsCreateInfoEXT layerSettingsInfo {
        .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .pNext        = gpuValidation && validationFeaturesEnabled ? &validationFeatures : nullptr,
        .settingCount = layerSettingsEnabled ? static_cast<uint32_t>(layerSettings.size()) : 0U,
        .pSettings    = layerSettingsEnabled ? layerSettings.data() : nullptr,
    };

    VkDebugUtilsMessengerCreateInfoEXT debugInfo {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext           = nullptr,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &Instance::DebugCallback,
        .pUserData       = result._debugState.get(),
    };

    const void* validationChain = nullptr;
    if (gpuValidation && validationFeaturesEnabled) {
        validationChain = layerSettingsEnabled ? static_cast<const void*>(&layerSettingsInfo) : static_cast<const void*>(&validationFeatures);
    }
    debugInfo.pNext = enableValidation ? validationChain : nullptr;

    VkInstanceCreateInfo createInfo {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = validation != ValidationMode::Off && debugUtilsEnabled ? &debugInfo : nullptr,
        .flags                   = 0,
        .pApplicationInfo        = &applicationInfo,
        .enabledLayerCount       = enableValidation ? 1U : 0U,
        .ppEnabledLayerNames     = enableValidation ? &kValidationLayerName : nullptr,
        .enabledExtensionCount   = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.empty() ? nullptr : enabledExtensions.data(),
    };

#if defined(__APPLE__)
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkInstance handle = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&createInfo, nullptr, &handle);
    if (created != VK_SUCCESS) {
        ZHLN::LogError("[Vulkan] vkCreateInstance failed: {}", static_cast<int32_t>(created));
        return result;
    }

    volkLoadInstance(handle);
    result._handle = handle;
    result._debugState->debugUtilsEnabled = debugUtilsEnabled;

    if (validation != ValidationMode::Off && debugUtilsEnabled && vkCreateDebugUtilsMessengerEXT != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = debugInfo;
        messengerInfo.pNext = nullptr;
        const VkResult messengerCreated = vkCreateDebugUtilsMessengerEXT(handle, &messengerInfo, nullptr, &result._messenger);
        if (messengerCreated != VK_SUCCESS) {
            result._messenger = VK_NULL_HANDLE;
            ZHLN::LogWarning("[Vulkan] Could not create validation debug messenger: {}", static_cast<int32_t>(messengerCreated));
        }

        if (vkCreateDebugUtilsMessengerEXT != nullptr) {
            const VkDebugUtilsMessengerCreateInfoEXT addressBindingInfo {
                .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext           = nullptr,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
                .pfnUserCallback = &Instance::DebugCallback,
                .pUserData       = result._debugState.get(),
            };
            const VkResult addressMessengerCreated = vkCreateDebugUtilsMessengerEXT(
                handle, &addressBindingInfo, nullptr, &result._addressBindingMessenger
            );
            if (addressMessengerCreated != VK_SUCCESS) {
                result._addressBindingMessenger = VK_NULL_HANDLE;
            }
        }
    }

    Instance* expected = nullptr;
    if (!_active.compare_exchange_strong(expected, &result, std::memory_order::release, std::memory_order::relaxed)) {
        ZHLN::LogError("[Vulkan] Only one active Vulkan instance is supported by the diagnostics bridge.");
        result.Destroy();
        return result;
    }

    return result;
}

auto Instance::ValidationErrorCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order::acquire);
    return active != nullptr ? active->_validationTarget->load(std::memory_order::relaxed) : 0;
}

auto Instance::DeviceLostCount() noexcept -> uint32_t {
    const Instance* const active = _active.load(std::memory_order::acquire);
    return active != nullptr ? active->_deviceLostTarget->load(std::memory_order::relaxed) : 0;
}

void Instance::IncrementNumericalDeviceLoss() noexcept {
    if (Instance* const active = _active.load(std::memory_order::acquire); active != nullptr) {
        active->_deviceLostTarget->fetch_add(1, std::memory_order::relaxed);
    }
}

} // namespace ZHLN::Vk
